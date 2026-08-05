#include <gtest/gtest.h>

#include <algorithm>  // std::shuffle, std::min
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/autograd/transformer.hpp"  // Adam
#include "zetta/compute/cpu_device.hpp"
#include "zetta/tokenizer/bpe.hpp"

#if defined(ZETTA_WITH_CUDA)
#include "zetta/compute/cuda_device.hpp"
#endif

// Device-agnostic SFT for the base agent (#918 corpus, #920 CUDA epic).
//
// The MLX driver in test_qwen_real_checkpoint.cpp is Apple-silicon only, which made it look as
// though nothing could train off a Mac. That was wrong, and the mistake is worth naming: the MLX
// path calls its loss `next_token_loss`, so searching for that name found nothing elsewhere and
// suggested no other training path existed. zetta's OWN autograd engine has had the whole loop all
// along — `cross_entropy` is the same objective under a different name — plus `Adam`, `zero_grad`,
// `backward()`, and `load_qwen_model` / `save_qwen_model`.
//
// So this driver needs no new kernels. `autograd::set_compute_device` routes the heavy op (matmul,
// forward AND backward) onto a ComputeDevice, so the SAME graph trains on CpuDevice or CudaDevice.
// Fused CUDA kernels (#922) make this faster; they were never a prerequisite for training on a GPU.
//
// Env-gated, never a build/CI dependency:
//   ZETTA_QWEN_DIR        the base checkpoint to fine-tune (required)
//   ZETTA_CORPUS          the gated corpus JSONL (required)
//   ZETTA_SFT_DEVICE      cpu | cuda   (default: cuda when built with CUDA, else cpu)
//   ZETTA_SFT_EXAMPLES    how many corpus cases to use (default 64)
//   ZETTA_SFT_STEPS       optimizer steps (default 20)
//   ZETTA_SFT_SAVE_EVERY  periodic checkpoint interval, 0 disables (default 250)
//   ZETTA_CHECKPOINT_OUT  where to write the trained checkpoint
//   ZETTA_SFT_LOG         metrics JSONL
namespace {

using zetta::agent::AdmissionCase;
using zetta::autograd::Adam;
using zetta::autograd::cross_entropy;
using zetta::autograd::QwenModel;
using zetta::autograd::Tensor;

const char* env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : fallback;
}

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::atoi(value) : fallback;
}

TEST(SftDeviceAgnostic, FineTunesTheBaseAgentOnAnyComputeDevice) {
    const char* model_dir = std::getenv("ZETTA_QWEN_DIR");
    if (model_dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";
    const char* corpus_path = std::getenv("ZETTA_CORPUS");
    if (corpus_path == nullptr) GTEST_SKIP() << "set ZETTA_CORPUS to a corpus JSONL to run";

    std::string error;

    // --- the device -------------------------------------------------------------------------
    // Chosen explicitly rather than implicitly, and REPORTED, because "did this actually run on
    // the GPU?" must never be a guess: a silent fallback to the CPU looks exactly like a slow GPU.
    std::string device_name = env_or("ZETTA_SFT_DEVICE", "");
    if (device_name.empty()) {
#if defined(ZETTA_WITH_CUDA)
        device_name = "cuda";
#else
        device_name = "cpu";
#endif
    }
    zetta::compute::CpuDevice cpu_device;
    zetta::compute::ComputeDevice* device = &cpu_device;
#if defined(ZETTA_WITH_CUDA)
    std::optional<zetta::compute::CudaDevice> cuda_device;
    if (device_name == "cuda") {
        cuda_device.emplace();
        const auto info = cuda_device->info();
        // A CudaDevice constructs even with no usable GPU; refuse to train rather than silently
        // fall back, so a run labelled "cuda" is always a run that used one.
        ASSERT_FALSE(info.name.empty() || info.name == "cuda")
            << "ZETTA_SFT_DEVICE=cuda but no CUDA device is usable";
        device = &*cuda_device;
        std::cerr << "[sft] device: cuda (" << info.name << ")\n";
    } else {
        std::cerr << "[sft] device: cpu\n";
    }
#else
    ASSERT_NE(device_name, "cuda") << "this build has no CUDA backend (-DZETTA_ENABLE_CUDA=ON)";
    std::cerr << "[sft] device: cpu\n";
#endif

    // --- model + tokenizer ------------------------------------------------------------------
    const std::optional<zetta::tokenizer::BpeTokenizer> tokenizer =
        zetta::tokenizer::BpeTokenizer::from_directory(model_dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    std::optional<QwenModel> model = zetta::autograd::load_qwen_model(model_dir, &error);
    ASSERT_TRUE(model.has_value()) << error;

    // --- corpus -----------------------------------------------------------------------------
    std::ifstream file(corpus_path);
    ASSERT_TRUE(file.good()) << "cannot open " << corpus_path;
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::optional<std::vector<AdmissionCase>> corpus =
        zetta::agent::load_corpus_jsonl(buffer.str(), &error);
    ASSERT_TRUE(corpus.has_value()) << error;
    // Re-gate before training on it: a corpus that cannot discriminate teaches nothing, and the
    // gate is cheap next to a fine-tune.
    ASSERT_TRUE(zetta::agent::admit_corpus(*corpus).admitted) << "corpus must pass the native gate";

    // Render each case with the SHARED renderer, so a training prompt is byte-identical to what
    // agent::respond will serve (#918). The target is the reference response envelope.
    const int max_examples = env_int("ZETTA_SFT_EXAMPLES", 64);
    std::vector<std::vector<int>> examples;
    for (const AdmissionCase& admission_case : *corpus) {
        if (static_cast<int>(examples.size()) >= max_examples) break;
        const auto [prompt, target] = zetta::agent::render_action_example(admission_case);
        std::vector<int> tokens = tokenizer->encode(prompt);
        const std::vector<int> target_tokens = tokenizer->encode(target);
        tokens.insert(tokens.end(), target_tokens.begin(), target_tokens.end());
        if (tokens.size() >= 4) examples.push_back(std::move(tokens));
    }
    ASSERT_FALSE(examples.empty());

    const int steps = env_int("ZETTA_SFT_STEPS", 20);
    const int save_every = env_int("ZETTA_SFT_SAVE_EVERY", 250);
    const std::string out_dir = env_or("ZETTA_CHECKPOINT_OUT", "sft-base-agent-autograd");
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    const std::string log_path = env_or("ZETTA_SFT_LOG", (out_dir + "/train.log").c_str());
    std::ofstream metrics(log_path);
    metrics << R"({"event":"start","device":")" << device_name << R"(","model":")" << model_dir
            << R"(","examples":)" << examples.size() << R"(,"steps":)" << steps << "}\n";
    metrics.flush();

    // --- train ------------------------------------------------------------------------------
    // Route the heavy op onto the chosen device for the whole loop, and restore afterwards: the
    // device is process-wide state, so leaving it set would leak into every later test.
    zetta::autograd::set_compute_device(device);
    struct DeviceReset {
        ~DeviceReset() { zetta::autograd::set_compute_device(nullptr); }
    } reset_device;

    Adam optimizer(/*learning_rate=*/1e-5F);
    // Shuffle, reshuffled per epoch. The corpus is grouped by family, so training it in order
    // feeds long runs of near-identical tasks and collapses onto whichever family lands last.
    std::mt19937 order_rng(1234U);
    std::vector<std::size_t> order(examples.size());
    std::iota(order.begin(), order.end(), 0U);
    std::shuffle(order.begin(), order.end(), order_rng);

    double first_loss = 0.0;
    double last_loss = 0.0;
    double min_loss = 1e30;
    for (int step = 0; step < steps; ++step) {
        const std::size_t position = static_cast<std::size_t>(step) % examples.size();
        if (position == 0 && step > 0) std::shuffle(order.begin(), order.end(), order_rng);
        const std::vector<int>& example = examples[order[position]];

        zetta::autograd::zero_grad(model->parameters());
        // Predict token t+1 from position t: drop the last row of logits and the first token.
        const Tensor logits = model->forward(example);
        const Tensor predicted =
            zetta::autograd::row_slice(logits, 0, static_cast<int>(example.size()) - 1);
        const std::vector<int> targets(example.begin() + 1, example.end());
        Tensor loss = cross_entropy(predicted, targets);
        loss.backward();
        optimizer.step(model->parameters());

        const double value = loss.data()[0];
        if (step == 0) first_loss = value;
        last_loss = value;
        min_loss = std::min(min_loss, value);
        metrics << R"({"step":)" << step << R"(,"loss":)" << value << "}\n";
        metrics.flush();  // durable per step
        if (step % 10 == 0) {
            std::cerr << "[sft] step " << step << "/" << steps << " loss " << value << "\n";
        }
        // Periodic checkpoint: an interruption should cost one interval, not the whole run
        // (a machine crash previously discarded ~90 steps because saving happened only at the end).
        if (save_every > 0 && step > 0 && step % save_every == 0) {
            std::string save_error;
            if (zetta::autograd::save_qwen_model(*model, out_dir, &save_error)) {
                metrics << R"({"event":"checkpoint","step":)" << step << "}\n";
                metrics.flush();
            } else {
                std::cerr << "[sft] checkpoint FAILED at step " << step << ": " << save_error
                          << "\n";
            }
        }
    }
    metrics << R"({"event":"done","first_loss":)" << first_loss << R"(,"last_loss":)" << last_loss
            << R"(,"min_loss":)" << min_loss << "}\n";
    metrics.flush();
    std::cerr << "[sft] " << examples.size() << " examples, " << steps << " steps on "
              << device_name << "; loss " << first_loss << " -> " << last_loss << " (min "
              << min_loss << ")\n";

    EXPECT_LT(min_loss, first_loss) << "SFT should reduce the loss";

    // The checkpoint must reload — a checkpoint that cannot be read back is not a checkpoint.
    ASSERT_TRUE(zetta::autograd::save_qwen_model(*model, out_dir, &error)) << error;
    const std::optional<QwenModel> reloaded = zetta::autograd::load_qwen_model(out_dir, &error);
    ASSERT_TRUE(reloaded.has_value()) << "the saved checkpoint must reload: " << error;
    std::cerr << "[sft] checkpoint saved + reloaded from " << out_dir << "\n";
}

}  // namespace
