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
#include "zetta/compute/mlx_device.hpp"
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
#endif  // ZETTA_WITH_MLX

}  // namespace
