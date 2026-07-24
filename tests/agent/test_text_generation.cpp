#include <gtest/gtest.h>

#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "zetta/agent/text_generation.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/autograd/transformer.hpp"
#include "zetta/tokenizer/bpe.hpp"
#include "zetta/tokenizer/byte_encoding.hpp"

namespace {

using zetta::agent::generate_text;
using zetta::autograd::Adam;
using zetta::autograd::cross_entropy;
using zetta::autograd::QwenConfig;
using zetta::autograd::QwenModel;
using zetta::autograd::row_slice;
using zetta::autograd::Tensor;
using zetta::autograd::zero_grad;
using zetta::tokenizer::BpeTokenizer;
using zetta::tokenizer::byte_to_unicode;
using zetta::tokenizer::utf8_encode;

// A byte-level tokenizer over all 256 bytes (ids 0..255) — no merges, so text
// tokenizes to its raw bytes. Pairs with a vocab_size >= 256 model.
BpeTokenizer byte_tokenizer() {
    std::unordered_map<std::string, int> vocab;
    for (int byte = 0; byte < 256; ++byte) {
        std::string symbol;
        utf8_encode(byte_to_unicode()[static_cast<std::size_t>(byte)], symbol);
        vocab[symbol] = byte;
    }
    return BpeTokenizer::from_vocab_and_merges(std::move(vocab), {}).value();
}

QwenConfig byte_config() {
    QwenConfig config;
    config.vocab_size = 256;  // one id per byte
    config.d_model = 32;
    config.n_layers = 2;
    config.n_heads = 4;
    config.n_kv_heads = 2;
    config.head_dim = 8;
    config.d_ff = 64;
    return config;
}

TEST(TextGeneration, GreedyIsDeterministicAndDecodesCleanly) {
    const QwenModel model(byte_config(), 5);
    const BpeTokenizer tokenizer = byte_tokenizer();
    const std::string a = generate_text(model, tokenizer, "hello", 4);
    const std::string b = generate_text(model, tokenizer, "hello", 4);
    EXPECT_EQ(a, b);  // greedy is deterministic
}

// END TO END, IN TEXT: overfit the model on a byte sequence, then generate_text
// from a text prefix reproduces the rest as text — tokenizer + model composed.
TEST(TextGeneration, OverfitModelContinuesTextGreedily) {
    QwenModel model(byte_config(), 3);
    const BpeTokenizer tokenizer = byte_tokenizer();

    // "orama" as byte ids; teach the model to memorize the next-byte sequence.
    const std::string word = "orama";
    const std::vector<int> ids = tokenizer.encode(word);
    ASSERT_EQ(ids.size(), word.size());
    const std::vector<int> targets(ids.begin() + 1, ids.end());

    Adam optimizer(/*learning_rate=*/1e-2F);
    for (int step = 0; step < 400; ++step) {
        zero_grad(model.parameters());
        const Tensor predicted =
            row_slice(model.forward(ids), 0, static_cast<int>(ids.size()) - 1);
        Tensor loss = cross_entropy(predicted, targets);
        loss.backward();
        optimizer.step(model.parameters());
    }

    // From "o", greedily generate the remaining 4 tokens → "rama".
    EXPECT_EQ(generate_text(model, tokenizer, "o", 4), "rama");
}

TEST(TextGeneration, SamplingStaysInVocabAndReproduces) {
    const QwenModel model(byte_config(), 9);
    const BpeTokenizer tokenizer = byte_tokenizer();
    std::mt19937 rng_a(42);
    std::mt19937 rng_b(42);
    const std::string a = generate_text(model, tokenizer, "hi", 5, 1.0F, rng_a);
    const std::string b = generate_text(model, tokenizer, "hi", 5, 1.0F, rng_b);
    EXPECT_EQ(a, b);  // same seed → same text
}

}  // namespace
