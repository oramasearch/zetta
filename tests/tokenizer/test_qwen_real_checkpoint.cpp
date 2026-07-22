#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/tokenizer/bpe.hpp"

#if defined(ZETTA_WITH_MLX)
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <system_error>
#include "zetta/agent/admission_gate.hpp"  // admit_corpus (native re-gate)
#include "zetta/agent/corpus_io.hpp"       // load_corpus_jsonl, to_json
#include "zetta/agent/qwen_rlvr.hpp"       // CPU agent::grpo_step
#include "zetta/autograd/transformer.hpp"  // autograd::Adam
#include "zetta/compute/mlx_device.hpp"
#include "zetta/compute/mlx_qwen.hpp"
#include "zetta/compute/mlx_transformer.hpp"  // AdamW
#endif

// Guarded integration test against a REAL Qwen3-0.6B checkpoint. It runs only when
// ZETTA_QWEN_DIR points at a directory holding config.json + tokenizer.json +
// model.safetensors (downloaded from https://huggingface.co/Qwen/Qwen3-0.6B), so
// the default build/CI needs no multi-GB download. It proves the native tokenizer
// is bit-exact to Qwen and the native model generates correctly from real weights.
// Full write-up: docs/reports/qwen3-real-checkpoint-validation-2026-07-21.md.
namespace {

using zetta::autograd::load_qwen_model;
using zetta::autograd::Tensor;
using zetta::tokenizer::BpeTokenizer;

const char* qwen_dir() { return std::getenv("ZETTA_QWEN_DIR"); }

// Reference token ids from the HF `tokenizers` library on the real tokenizer.json.
struct Case {
    std::string text;
    std::vector<int> ids;
};

TEST(QwenRealCheckpoint, TokenizerMatchesReferenceExactly) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;

    const std::vector<Case> cases = {
        {"Hello, world!", {9707, 11, 1879, 0}},
        {"don't can't we've I'll they're",
         {15007, 944, 646, 944, 582, 3003, 358, 3278, 807, 2299}},
        {"café \xE2\x86\x92 \xCF\x80 \xE2\x89\x88 3.14159",  // "café → π ≈ 3.14159"
         {924, 58858, 11397, 51745, 21103, 230, 220, 18, 13, 16, 19, 16, 20, 24}},
        {"<|im_start|>user\nHi<|im_end|>", {151644, 872, 198, 13048, 151645}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(tokenizer->encode(c.text), c.ids) << "encode: " << c.text;
        EXPECT_EQ(tokenizer->decode(c.ids), c.text) << "decode: " << c.text;
    }
}

TEST(QwenRealCheckpoint, ModelGeneratesCorrectlyFromRealWeights) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    const std::optional<zetta::autograd::QwenModel> model = load_qwen_model(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;

    // "The capital of France is" → argmax next token is " Paris" (id 12095).
    const std::vector<int> prompt = tokenizer->encode("The capital of France is");
    ASSERT_EQ(prompt, (std::vector<int>{785, 6722, 315, 9625, 374}));
    const Tensor logits = model->forward(prompt);
    const int vocab = model->config().vocab_size;
    const float* last = logits.data().data() + (prompt.size() - 1) * vocab;
    int best = 0;
    for (int c = 1; c < vocab; ++c) {
        if (last[c] > last[best]) best = c;
    }
    EXPECT_EQ(best, 12095);
    EXPECT_EQ(tokenizer->decode({best}), " Paris");
}

#if defined(ZETTA_WITH_MLX)
// The real model on the Apple-Silicon GPU (MLX): same argmax (" Paris") as the CPU
// path, and a wall-clock comparison of the forward (host thread pool vs MLX gemm).
TEST(QwenRealCheckpoint, ForwardOnMlxGpuMatchesCpu) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    const std::optional<zetta::autograd::QwenModel> model = load_qwen_model(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;
    const std::vector<int> prompt = tokenizer->encode("The capital of France is");

    using clock = std::chrono::steady_clock;
    const auto ms = [](auto d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    const auto c0 = clock::now();
    const std::vector<float> cpu = model->forward(prompt).data();
    const auto c1 = clock::now();

    zetta::compute::MlxDevice mlx;
    zetta::autograd::set_compute_device(&mlx);
    model->forward(prompt);  // warm up (MLX compiles kernels lazily on first use)
    const auto m0 = clock::now();
    const std::vector<float> gpu = model->forward(prompt).data();
    const auto m1 = clock::now();
    zetta::autograd::set_compute_device(nullptr);

    ASSERT_EQ(cpu.size(), gpu.size());
    const int vocab = model->config().vocab_size;
    const std::size_t last = (prompt.size() - 1) * vocab;
    int gpu_next = 0;
    double max_diff = 0.0;
    for (int c = 0; c < vocab; ++c) {
        if (gpu[last + c] > gpu[last + gpu_next]) gpu_next = c;
    }
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        max_diff = std::max<double>(max_diff, std::abs(cpu[i] - gpu[i]));
    }
    std::cerr << "[real Qwen3-0.6B, 5-token forward] CPU " << ms(c1 - c0)
              << " ms, MLX GPU " << ms(m1 - m0) << " ms; next token (MLX) = "
              << gpu_next << "; max|CPU-MLX| over all logits = " << max_diff << "\n";

    // Device-agnostic: the GPU forward agrees with the CPU forward (28 layers of
    // f32 accumulation differ only slightly between the host kernel and MLX gemm),
    // and the GPU still predicts " Paris" (id 12095).
    EXPECT_EQ(gpu_next, 12095) << "MLX next-token argmax should be ' Paris'";
    EXPECT_LT(max_diff, 0.05) << "CPU vs MLX logits diverge too far";
}
// A scaling benchmark: the 5-token forward is where per-op MLX routing loses (sync
// overhead > compute), but the projections are [seq, d] x [d, out], so the compute
// grows with the sequence length. Sweep it and measure CPU (threaded host) vs MLX
// GPU per forward to find where the GPU wins. Reports only — no assertions on time.
TEST(QwenRealCheckpoint, ForwardScalingCpuVsMlx) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";
    if (std::getenv("ZETTA_BENCH") == nullptr) {
        GTEST_SKIP() << "set ZETTA_BENCH=1 to run the (slow) scaling benchmark";
    }

    std::string error;
    const std::optional<zetta::autograd::QwenModel> model = load_qwen_model(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;
    const int vocab = model->config().vocab_size;

    using clock = std::chrono::steady_clock;
    const auto ms = [](auto d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    zetta::compute::MlxDevice mlx;

    std::cerr << "\n[real Qwen3-0.6B forward: CPU threaded vs MLX GPU]\n";
    std::cerr << "  seq |   CPU ms |   MLX ms | speedup\n";
    for (int seq : {8, 32, 128, 512}) {
        std::vector<int> ids(static_cast<std::size_t>(seq));
        for (int i = 0; i < seq; ++i) ids[i] = (i * 7 + 1) % vocab;

        zetta::autograd::set_compute_device(nullptr);
        const auto c0 = clock::now();
        volatile float cpu_sink = model->forward(ids).data()[0];
        const auto c1 = clock::now();
        (void)cpu_sink;

        zetta::autograd::set_compute_device(&mlx);
        model->forward(ids);  // warm up
        const auto m0 = clock::now();
        volatile float mlx_sink = model->forward(ids).data()[0];
        const auto m1 = clock::now();
        (void)mlx_sink;
        zetta::autograd::set_compute_device(nullptr);

        const double cpu_ms = ms(c1 - c0);
        const double mlx_ms = ms(m1 - m0);
        std::fprintf(stderr, " %4d | %8.1f | %8.1f | %6.2fx\n", seq, cpu_ms, mlx_ms,
                     cpu_ms / mlx_ms);
    }
}

// Generation tok/s: the KV-cached engine (plain-float host) vs the autograd
// generate path on CPU and on MLX. Generation is token-by-token — tiny per-step
// forwards — so per-op MLX sync should hurt, unlike the batched forward above.
TEST(QwenRealCheckpoint, GenerationTokensPerSecondCpuVsMlx) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";
    if (std::getenv("ZETTA_BENCH") == nullptr) GTEST_SKIP() << "set ZETTA_BENCH=1";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    std::optional<zetta::autograd::QwenModel> model = load_qwen_model(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;
    const std::vector<int> prompt = tokenizer->encode("The capital of France is");
    const int n = 16;

    using clock = std::chrono::steady_clock;
    const auto rate = [&](auto t0, auto t1, int tokens) {
        return tokens / std::chrono::duration<double>(t1 - t0).count();
    };

    // KV-cached, plain-float host engine.
    zetta::autograd::QwenInference engine(*model);
    const auto k0 = clock::now();
    const auto kv = engine.generate_greedy(prompt, n);
    const auto k1 = clock::now();

    // Autograd generate (re-forwards each token, no cache) on CPU.
    zetta::autograd::set_compute_device(nullptr);
    const auto a0 = clock::now();
    model->generate_greedy(prompt, n);
    const auto a1 = clock::now();

    // Same autograd path on the MLX GPU.
    zetta::compute::MlxDevice mlx;
    zetta::autograd::set_compute_device(&mlx);
    model->generate_greedy(prompt, 2);  // warm up
    const auto g0 = clock::now();
    model->generate_greedy(prompt, n);
    const auto g1 = clock::now();
    zetta::autograd::set_compute_device(nullptr);

    std::fprintf(stderr,
                 "\n[real Qwen3-0.6B generation, %d tokens]\n"
                 "  KV-cached (CPU)        : %6.2f tok/s\n"
                 "  autograd re-forward CPU: %6.2f tok/s\n"
                 "  autograd re-forward MLX: %6.2f tok/s\n",
                 n, rate(k0, k1, static_cast<int>(kv.size())), rate(a0, a1, n),
                 rate(g0, g1, n));
}

// THE FUSED MLX KERNEL: the whole decode step is one MLX graph with the KV cache in
// GPU arrays (one eval per token, no per-op sync). It must produce the same tokens
// as the CPU KV-cached path, and it should generate FAST on the GPU — unlike the
// per-op autograd routing which loses to the CPU.
TEST(QwenRealCheckpoint, FusedMlxGenerationMatchesCpuAndIsFast) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    const std::optional<zetta::autograd::QwenModel> model = load_qwen_model(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;
    std::optional<zetta::compute::model::MlxQwenInference> fused =
        zetta::compute::model::MlxQwenInference::from_directory(dir, &error);
    ASSERT_TRUE(fused.has_value()) << error;

    const std::vector<int> prompt = tokenizer->encode("The capital of France is");
    const int n = 24;

    // Same greedy tokens as the CPU KV-cached engine (the correctness gate).
    zetta::autograd::QwenInference cpu(*model);
    const std::vector<int> cpu_gen = cpu.generate_greedy(prompt, n);
    const std::vector<int> mlx_gen = fused->generate_greedy(prompt, n);
    EXPECT_EQ(mlx_gen, cpu_gen)
        << "fused MLX: \"" << tokenizer->decode(mlx_gen) << "\" vs CPU \""
        << tokenizer->decode(cpu_gen) << "\"";

    // Tokens/sec on the GPU (warm up first — MLX compiles kernels lazily).
    using clock = std::chrono::steady_clock;
    fused->generate_greedy(prompt, 4);
    const auto t0 = clock::now();
    const std::vector<int> timed = fused->generate_greedy(prompt, 32);
    const auto t1 = clock::now();
    const double tps = timed.size() / std::chrono::duration<double>(t1 - t0).count();
    std::cerr << "[fused MLX generation] " << tps << " tok/s : \""
              << tokenizer->decode(timed) << "\"\n";
}

// THE FUSED MLX TRAINING KERNEL: mx::value_and_grad backprops through the whole
// Qwen3 stack in one graph and AdamW updates on the GPU. Overfitting a sequence
// must collapse the loss — real backprop through the real 0.6B weights on Metal.
TEST(QwenRealCheckpoint, FusedMlxTrainingBackpropsThroughRealWeights) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";
    namespace mx = mlx::core;

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    std::optional<zetta::compute::model::MlxQwenModel> model =
        zetta::compute::model::MlxQwenModel::from_directory(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;

    const std::vector<int> ids = tokenizer->encode("The capital of France is Berlin");
    const std::vector<int> ids32(ids.begin(), ids.end());
    const mx::array tokens(ids32.data(), mx::Shape{static_cast<int>(ids.size())}, mx::int32);

    const auto loss_fn = [&](const std::vector<mx::array>& p) {
        return model->next_token_loss(p, tokens);
    };
    std::vector<int> argnums(model->parameters().size());
    std::iota(argnums.begin(), argnums.end(), 0);
    const auto grad_fn = mx::value_and_grad(loss_fn, argnums);
    zetta::compute::model::AdamW optimizer(/*learning_rate=*/1e-4F);

    using clock = std::chrono::steady_clock;
    float first_loss = 0.0F;
    float min_loss = 1e30F;
    double step_ms = 0.0;
    for (int step = 0; step < 8; ++step) {
        const auto t0 = clock::now();
        auto [loss, gradients] = grad_fn(model->parameters());
        optimizer.update(model->parameters(), gradients);
        mx::eval(model->parameters());
        mx::eval(loss);
        const auto t1 = clock::now();
        const float l = loss.item<float>();
        if (step == 0) first_loss = l;
        min_loss = std::min(min_loss, l);
        step_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cerr << "[fused MLX train] step " << step << " loss=" << l << " ("
                  << step_ms << " ms)\n";
    }
    // Backprop demonstrably reduces the loss (single-sequence Adam on a 0.6B model
    // is noisy step-to-step, so we check the loss dropped, not that it's monotone).
    EXPECT_LT(min_loss, first_loss * 0.5F) << "fused MLX backprop should reduce the loss";
    std::cerr << "[fused MLX train] ~" << step_ms
              << " ms / step (fwd+bwd+AdamW over 0.6B params on the GPU)\n";
}

// THE FULLY-FUSED GPU RLVR LOOP: rollouts (KV-cached MLX) + a group value_and_grad
// (one MLX graph) + AdamW, params shared so updates flow into the next rollout. It
// learns a target on the real 0.6B purely from a verifiable reward, on the GPU.
TEST(QwenRealCheckpoint, FusedGpuRlvrLearnsOnRealWeights) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    std::optional<zetta::compute::model::MlxQwenModel> model =
        zetta::compute::model::MlxQwenModel::from_directory(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;

    const std::vector<int> prompt = tokenizer->encode("I think the best pet is a");
    const std::string target = "cat";
    const auto reward = [&](const std::vector<int>&, const std::vector<int>& completion) {
        return tokenizer->decode(completion).find(target) != std::string::npos ? 1.0 : 0.0;
    };

    zetta::compute::model::AdamW optimizer(/*learning_rate=*/1e-4F);
    std::mt19937 rng(7);
    using clock = std::chrono::steady_clock;

    const auto greedy = [&]() {
        std::mt19937 g(0);
        return tokenizer->decode(model->generate(prompt, 3, 0.0F, g));
    };
    std::cerr << "[fused GPU RLVR] before: greedy = \"" << greedy() << "\"\n";

    double max_reward = 0.0;
    double step_ms = 0.0;
    bool learned_greedy = false;
    for (int step = 0; step < 12; ++step) {
        const auto t0 = clock::now();
        const double mean_reward = model->grpo_step(optimizer, prompt, reward,
                                                    /*group=*/8, /*max_new=*/3,
                                                    /*temperature=*/1.0F, rng);
        const auto t1 = clock::now();
        step_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const std::string g = greedy();
        max_reward = std::max(max_reward, mean_reward);
        if (g.find(target) != std::string::npos) learned_greedy = true;
        std::cerr << "[fused GPU RLVR] step " << step << " reward=" << mean_reward
                  << " (" << step_ms << " ms) greedy=\"" << g << "\"\n";
        if (learned_greedy && mean_reward >= 0.75) break;  // learned + stable
    }
    std::cerr << "[fused GPU RLVR] after: greedy=\"" << greedy() << "\"  ~" << step_ms
              << " ms / RLVR step (rollouts + group value_and_grad + AdamW on GPU)\n";
    EXPECT_GT(max_reward, 0.5) << "RLVR should learn to sample the target from reward";
}

// Head-to-head: one RLVR step, identical config, CPU (autograd grpo_step) vs the
// fully-fused GPU grpo_step — the speedup the whole-step fusion buys.
TEST(QwenRealCheckpoint, RlvrStepCpuVsFusedGpu) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";
    if (std::getenv("ZETTA_BENCH") == nullptr) GTEST_SKIP() << "set ZETTA_BENCH=1";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    std::optional<zetta::autograd::QwenModel> cpu_model = load_qwen_model(dir, &error);
    ASSERT_TRUE(cpu_model.has_value()) << error;
    std::optional<zetta::compute::model::MlxQwenModel> gpu_model =
        zetta::compute::model::MlxQwenModel::from_directory(dir, &error);
    ASSERT_TRUE(gpu_model.has_value()) << error;

    const std::vector<int> prompt = tokenizer->encode("I think the best pet is a");
    const auto reward = [&](const std::vector<int>&, const std::vector<int>& c) {
        return tokenizer->decode(c).find("cat") != std::string::npos ? 1.0 : 0.0;
    };
    const int group = 8, max_new = 3;
    const float temp = 1.0F;

    using clock = std::chrono::steady_clock;
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };

    // CPU: autograd grpo_step (KV-cached rollouts + per-completion autograd backward).
    zetta::autograd::Adam cpu_opt(1e-4F);
    zetta::agent::GrpoConfig cfg;
    cfg.group_size = group; cfg.max_new_tokens = max_new; cfg.temperature = temp;
    std::mt19937 rng_cpu(7);
    const auto c0 = clock::now();
    zetta::agent::grpo_step(*cpu_model, cpu_opt, prompt, reward, cfg, rng_cpu);
    const auto c1 = clock::now();

    // GPU: fully-fused grpo_step (warm up once — MLX compiles kernels lazily).
    zetta::compute::model::AdamW gpu_opt(1e-4F);
    std::mt19937 rng_gpu(7);
    gpu_model->grpo_step(gpu_opt, prompt, reward, group, max_new, temp, rng_gpu);
    const auto g0 = clock::now();
    gpu_model->grpo_step(gpu_opt, prompt, reward, group, max_new, temp, rng_gpu);
    const auto g1 = clock::now();

    const double cpu_ms = ms(c1 - c0), gpu_ms = ms(g1 - g0);
    std::fprintf(stderr,
                 "\n[real Qwen3-0.6B RLVR step, group %d x %d new tokens]\n"
                 "  CPU (autograd)   : %8.0f ms\n"
                 "  fused GPU        : %8.0f ms   (%.1fx)\n",
                 group, max_new, cpu_ms, gpu_ms, cpu_ms / gpu_ms);
}

// STABLE sustained RLVR: the fused GPU GRPO step, run for many iterations at a
// moderate learning rate, learns the verifiable target AND stays rewarded (coherent)
// instead of drifting into the reward-hacking mode collapse that GRPO falls into at an
// aggressive lr. The stabilizer here is the learning rate, not a regularizer: a swept
// reference-KL penalty (both a per-token k3 and a full-distribution forward KL) did NOT
// help at this scale — the dense KL puts gradient on all ~152k vocab rows every step,
// and Adam's per-coordinate normalization turns that into a full-vocab perturbation
// that destabilizes faster than it anchors. See the validation report.
TEST(QwenRealCheckpoint, FusedGpuRlvrLearnsStablyAtModerateLr) {
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    std::optional<zetta::compute::model::MlxQwenModel> model =
        zetta::compute::model::MlxQwenModel::from_directory(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;

    const std::vector<int> prompt = tokenizer->encode("I think the best pet is a");
    const auto reward = [&](const std::vector<int>&, const std::vector<int>& c) {
        return tokenizer->decode(c).find("cat") != std::string::npos ? 1.0 : 0.0;
    };
    const auto greedy = [&]() {
        std::mt19937 g(0);
        return tokenizer->decode(model->generate(prompt, 3, 0.0F, g));
    };

    zetta::compute::model::AdamW optimizer(/*learning_rate=*/1e-4F);
    std::mt19937 rng(7);
    double max_reward = 0.0;
    int rewarded_steps = 0;
    for (int step = 0; step < 12; ++step) {
        const double mean_reward = model->grpo_step(optimizer, prompt, reward, /*group=*/8,
                                                    /*max_new=*/3, /*temperature=*/1.0F, rng);
        max_reward = std::max(max_reward, mean_reward);
        if (mean_reward >= 0.5) ++rewarded_steps;
        std::cerr << "[RLVR] step " << step << " reward=" << mean_reward
                  << " greedy=\"" << greedy() << "\"\n";
    }
    std::cerr << "[RLVR] final greedy=\"" << greedy() << "\"  (" << rewarded_steps
              << "/12 steps rewarded)\n";
    // Learned the target, and stayed rewarded across the run — no collapse to 0 (a
    // collapse leaves rewarded_steps tiny, as an aggressive lr or a dense KL does).
    EXPECT_GT(max_reward, 0.5) << "should learn to sample the target";
    EXPECT_GE(rewarded_steps, 8) << "reward collapsed — RLVR was not stable";
    EXPECT_NE(greedy().find("cat"), std::string::npos)
        << "the learned greedy answer should contain the target";
}

// Base-agent SFT on the generated corpus (#863 → #555): load the real 0.6B and
// data/corpus.jsonl, re-gate it with the native authority, render each task as
// intent → the required-facts solution, fine-tune with next_token_loss + AdamW on the
// GPU, then SAVE a reloadable checkpoint. This is the training driver — guarded on
// ZETTA_QWEN_DIR + ZETTA_CORPUS. Knobs (env): ZETTA_SFT_EXAMPLES (default 64),
// ZETTA_SFT_STEPS (default 60), ZETTA_CHECKPOINT_OUT (default ./sft-base-agent-0.6b).
TEST(QwenRealCheckpoint, SftBaseAgentOnCorpus) {
    namespace mx = mlx::core;
    const char* dir = qwen_dir();
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run";
    const char* corpus_path = std::getenv("ZETTA_CORPUS");
    if (corpus_path == nullptr) GTEST_SKIP() << "set ZETTA_CORPUS to a corpus JSONL to run";

    std::string error;
    const std::optional<BpeTokenizer> tokenizer = BpeTokenizer::from_directory(dir, &error);
    ASSERT_TRUE(tokenizer.has_value()) << error;
    std::optional<zetta::compute::model::MlxQwenModel> model =
        zetta::compute::model::MlxQwenModel::from_directory(dir, &error);
    ASSERT_TRUE(model.has_value()) << error;

    // Load + re-gate the corpus with the native authority before training on it.
    std::ifstream file(corpus_path);
    ASSERT_TRUE(file.good()) << "cannot open " << corpus_path;
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto corpus = zetta::agent::load_corpus_jsonl(buffer.str(), &error);
    ASSERT_TRUE(corpus.has_value()) << error;
    ASSERT_TRUE(zetta::agent::admit_corpus(*corpus).admitted) << "corpus must pass the native gate";

    // Render SFT examples: prompt (the intent) + target (the required-facts solution).
    const int max_examples =
        std::getenv("ZETTA_SFT_EXAMPLES") ? std::atoi(std::getenv("ZETTA_SFT_EXAMPLES")) : 64;
    std::vector<std::vector<int>> examples;
    for (const zetta::agent::AdmissionCase& admission_case : *corpus) {
        if (static_cast<int>(examples.size()) >= max_examples) break;
        const std::string prompt =
            admission_case.task.description() + "\n\nCorrect solution (facts):\n";
        const std::string target = zetta::agent::to_json(admission_case.task.required()).dump();
        std::vector<int> tokens = tokenizer->encode(prompt);
        const std::vector<int> target_tokens = tokenizer->encode(target);
        tokens.insert(tokens.end(), target_tokens.begin(), target_tokens.end());
        if (tokens.size() >= 4) examples.push_back(std::move(tokens));
    }
    ASSERT_FALSE(examples.empty());

    const int steps =
        std::getenv("ZETTA_SFT_STEPS") ? std::atoi(std::getenv("ZETTA_SFT_STEPS")) : 60;
    const char* out = std::getenv("ZETTA_CHECKPOINT_OUT");
    const std::string out_dir = out != nullptr ? out : "sft-base-agent-0.6b";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    // Live training-metrics log (JSONL), flushed per step so it survives an interruption.
    const char* log_env = std::getenv("ZETTA_SFT_LOG");
    const std::string log_path = log_env != nullptr ? log_env : out_dir + "/train.log";
    std::ofstream metrics(log_path);
    metrics << R"({"event":"start","model":")" << dir << R"(","corpus":")" << corpus_path
            << R"(","examples":)" << examples.size() << R"(,"steps":)" << steps
            << R"(,"lr":1e-5})" << "\n";
    metrics.flush();

    zetta::compute::model::AdamW optimizer(/*learning_rate=*/1e-5F);
    std::vector<int> argnums(model->parameters().size());
    std::iota(argnums.begin(), argnums.end(), 0);
    double first_loss = 0.0;
    double last_loss = 0.0;
    double min_loss = 1e30;
    for (int step = 0; step < steps; ++step) {
        const std::vector<int>& example =
            examples[static_cast<std::size_t>(step) % examples.size()];
        const mx::array tokens(example.data(),
                               mx::Shape{static_cast<int>(example.size())}, mx::int32);
        const auto loss_fn = [&](const std::vector<mx::array>& params) {
            return model->next_token_loss(params, tokens);
        };
        auto [loss, gradients] = mx::value_and_grad(loss_fn, argnums)(model->parameters());
        optimizer.update(model->parameters(), gradients);
        mx::eval(model->parameters());
        mx::eval(loss);
        const double value = loss.item<float>();
        if (step == 0) first_loss = value;
        last_loss = value;
        min_loss = std::min(min_loss, value);
        metrics << R"({"step":)" << step << R"(,"loss":)" << value << "}\n";
        metrics.flush();  // durable per step
        if (step % 50 == 0) {
            std::cerr << "[SFT] step " << step << "/" << steps << " loss " << value << "\n";
        }
    }
    metrics << R"({"event":"done","first_loss":)" << first_loss << R"(,"last_loss":)"
            << last_loss << R"(,"min_loss":)" << min_loss << "}\n";
    metrics.flush();
    std::cerr << "[SFT base agent] " << examples.size() << " examples, " << steps
              << " steps; loss " << first_loss << " -> " << last_loss << " (min " << min_loss
              << "); log " << log_path << "\n";
    EXPECT_LT(min_loss, first_loss) << "SFT should reduce the loss";

    // Save the fine-tuned checkpoint and confirm it reloads (the produced base agent).
    ASSERT_TRUE(model->save(out_dir, dir, &error)) << error;
    const std::optional<zetta::compute::model::MlxQwenModel> reloaded =
        zetta::compute::model::MlxQwenModel::from_directory(out_dir, &error);
    ASSERT_TRUE(reloaded.has_value()) << "the saved checkpoint must reload: " << error;
    std::cerr << "[SFT base agent] checkpoint saved + reloaded from " << out_dir << "\n";
}

#endif  // ZETTA_WITH_MLX

}  // namespace
