#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/safetensors.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/autograd/transformer.hpp"

namespace {

using zetta::autograd::Adam;
using zetta::autograd::cross_entropy;
using zetta::autograd::load_qwen_model;
using zetta::autograd::parse_qwen_config;
using zetta::autograd::QwenConfig;
using zetta::autograd::QwenModel;
using zetta::autograd::read_safetensors_file;
using zetta::autograd::row_slice;
using zetta::autograd::SafeTensor;
using zetta::autograd::SafeTensors;
using zetta::autograd::Tensor;
using zetta::autograd::write_safetensors_file;
using zetta::autograd::zero_grad;

QwenConfig tiny_config() {
    QwenConfig config;
    config.vocab_size = 24;
    config.d_model = 16;
    config.n_layers = 2;
    config.n_heads = 4;
    config.n_kv_heads = 2;
    config.head_dim = 4;
    config.d_ff = 32;
    return config;
}

TEST(QwenModel, ForwardProducesFiniteLogitsOfTheRightShape) {
    const QwenModel model(tiny_config(), 42);
    const Tensor logits = model.forward({1, 5, 9, 3, 7});
    ASSERT_EQ(logits.shape(), (std::vector<int>{5, 24}));
    for (float value : logits.data()) {
        EXPECT_TRUE(std::isfinite(value));
    }
}

TEST(QwenModel, ForwardIsDeterministic) {
    const QwenModel model(tiny_config(), 7);
    const Tensor a = model.forward({2, 4, 6, 8});
    const Tensor b = model.forward({2, 4, 6, 8});
    EXPECT_EQ(a.data(), b.data());
}

// The HF tensor names resolve to parameters of the shapes Qwen3 uses.
TEST(QwenModel, ExposesParametersByHuggingFaceName) {
    const QwenConfig config = tiny_config();
    const QwenModel model(config, 1);
    const int q_dim = config.n_heads * config.head_dim;    // 16
    const int kv_dim = config.n_kv_heads * config.head_dim;  // 8

    EXPECT_EQ(model.weight("model.embed_tokens.weight").shape(),
              (std::vector<int>{config.vocab_size, config.d_model}));
    EXPECT_EQ(model.weight("model.layers.0.self_attn.q_proj.weight").shape(),
              (std::vector<int>{q_dim, config.d_model}));
    EXPECT_EQ(model.weight("model.layers.0.self_attn.k_proj.weight").shape(),
              (std::vector<int>{kv_dim, config.d_model}));
    EXPECT_EQ(model.weight("model.layers.1.self_attn.q_norm.weight").shape(),
              (std::vector<int>{config.head_dim}));
    EXPECT_EQ(model.weight("model.layers.1.mlp.down_proj.weight").shape(),
              (std::vector<int>{config.d_model, config.d_ff}));
    EXPECT_EQ(model.weight("model.norm.weight").shape(),
              (std::vector<int>{config.d_model}));
    EXPECT_EQ(model.weight("lm_head.weight").shape(),
              (std::vector<int>{config.vocab_size, config.d_model}));

    EXPECT_FALSE(model.weight("no.such.tensor").valid());
}

// Every parameter is an autograd leaf; the count matches the architecture.
TEST(QwenModel, ParameterCountMatchesArchitecture) {
    const QwenConfig config = tiny_config();
    const QwenModel model(config, 1);
    // embed + (11 per layer) + final norm + lm_head.
    const std::size_t expected = 1 + 11 * config.n_layers + 1 + 1;
    EXPECT_EQ(model.parameters().size(), expected);
}

// set_weight loads a checkpoint tensor in place: it changes the forward output,
// and rejects unknown names and size mismatches (a non-throwing boundary).
TEST(QwenModel, SetWeightLoadsInPlaceAndValidates) {
    QwenModel model(tiny_config(), 3);
    const std::vector<float> before = model.forward({1, 2, 3}).data();

    const std::size_t norm_size = model.weight("model.norm.weight").data().size();
    std::vector<float> new_norm(norm_size, 2.0F);  // scale the final norm up
    EXPECT_TRUE(model.set_weight("model.norm.weight", new_norm));
    EXPECT_EQ(model.weight("model.norm.weight").data(), new_norm);

    const std::vector<float> after = model.forward({1, 2, 3}).data();
    ASSERT_EQ(before.size(), after.size());
    float max_diff = 0.0F;
    for (std::size_t i = 0; i < before.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(before[i] - after[i]));
    }
    EXPECT_GT(max_diff, 1e-5F);  // the loaded weight actually took effect

    EXPECT_FALSE(model.set_weight("no.such.tensor", new_norm));       // unknown name
    EXPECT_FALSE(model.set_weight("model.norm.weight", {1.0F, 2.0F}));  // wrong size
}

// With tied embeddings there is no lm_head parameter, but the forward still
// produces logits of the right shape (it reuses embed_tokens as the projection).
TEST(QwenModel, TiedEmbeddingsHaveNoLmHeadButStillProject) {
    QwenConfig config = tiny_config();
    config.tie_word_embeddings = true;
    const QwenModel model(config, 5);
    EXPECT_FALSE(model.weight("lm_head.weight").valid());
    const Tensor logits = model.forward({1, 2, 3, 4});
    ASSERT_EQ(logits.shape(), (std::vector<int>{4, config.vocab_size}));
    for (float value : logits.data()) {
        EXPECT_TRUE(std::isfinite(value));
    }
}

// A Hugging Face config.json (Qwen3 field names) builds the matching QwenConfig,
// so the right architecture is instantiated from a checkpoint.
TEST(QwenModel, ParsesHuggingFaceConfigJson) {
    const std::string json = R"({
        "architectures": ["Qwen3ForCausalLM"],
        "vocab_size": 151936,
        "hidden_size": 1024,
        "num_hidden_layers": 28,
        "num_attention_heads": 16,
        "num_key_value_heads": 8,
        "head_dim": 128,
        "intermediate_size": 3072,
        "rms_norm_eps": 1e-6,
        "rope_theta": 1000000.0,
        "tie_word_embeddings": true
    })";
    const std::optional<QwenConfig> config = parse_qwen_config(json);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->vocab_size, 151936);
    EXPECT_EQ(config->d_model, 1024);
    EXPECT_EQ(config->n_layers, 28);
    EXPECT_EQ(config->n_heads, 16);
    EXPECT_EQ(config->n_kv_heads, 8);
    EXPECT_EQ(config->head_dim, 128);
    EXPECT_EQ(config->d_ff, 3072);
    EXPECT_FLOAT_EQ(config->rope_base, 1000000.0F);
    EXPECT_TRUE(config->tie_word_embeddings);
}

// head_dim falls back to hidden_size / num_attention_heads when the config omits
// it (older HF configs), and rope/eps take Qwen3 defaults.
TEST(QwenModel, ConfigHeadDimFallsBackAndDefaultsApply) {
    const std::string json = R"({
        "vocab_size": 100,
        "hidden_size": 64,
        "num_hidden_layers": 2,
        "num_attention_heads": 8,
        "num_key_value_heads": 2,
        "intermediate_size": 128
    })";
    const std::optional<QwenConfig> config = parse_qwen_config(json);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->head_dim, 8);  // 64 / 8
    EXPECT_FALSE(config->tie_word_embeddings);
    EXPECT_FLOAT_EQ(config->rope_base, QwenConfig{}.rope_base);  // default kept

    // A config that parses can actually build a model.
    const QwenModel model(*config, 1);
    EXPECT_EQ(model.forward({1, 2, 3}).shape(), (std::vector<int>{3, config->vocab_size}));
}

TEST(QwenModel, RejectsMalformedConfig) {
    std::string error;
    EXPECT_FALSE(parse_qwen_config("not json", &error).has_value());
    EXPECT_FALSE(error.empty());

    // Missing a required field.
    EXPECT_FALSE(parse_qwen_config(R"({"hidden_size": 64})", &error).has_value());
    EXPECT_NE(error.find("vocab_size"), std::string::npos);

    // Inconsistent head counts (n_heads not a multiple of n_kv_heads).
    const std::string bad = R"({
        "vocab_size": 100, "hidden_size": 64, "num_hidden_layers": 2,
        "num_attention_heads": 7, "num_key_value_heads": 2,
        "head_dim": 8, "intermediate_size": 128
    })";
    EXPECT_FALSE(parse_qwen_config(bad, &error).has_value());
    EXPECT_NE(error.find("multiple"), std::string::npos);
}

// THE ENTRY POINT: point load_qwen_model at a Hugging Face checkpoint directory
// (config.json + model.safetensors) and get a fully-loaded, ready-to-run model.
// Proven here with a synthetic directory whose config matches tiny_config; a real
// Qwen directory is the same two files (only the weights + tokenizer differ, and
// those are the external-asset step).
TEST(QwenModel, LoadsFromACheckpointDirectory) {
    const QwenConfig config = tiny_config();  // vocab 24, d 16, 2 layers, GQA 4/2
    const QwenModel source(config, 11);
    const std::vector<int> ids = {1, 5, 2, 7};
    const std::vector<float> expected = source.forward(ids).data();

    const zetta::test::TempDir temp;
    // config.json matching tiny_config.
    std::ofstream(temp.path() / "config.json")
        << R"({"vocab_size":24,"hidden_size":16,"num_hidden_layers":2,)"
           R"("num_attention_heads":4,"num_key_value_heads":2,"head_dim":4,)"
           R"("intermediate_size":32,"rms_norm_eps":1e-6,"rope_theta":1000000.0,)"
           R"("tie_word_embeddings":false})";
    ASSERT_TRUE(write_safetensors_file((temp.path() / "model.safetensors").string(),
                                       source.to_safetensors()));

    std::string error;
    const std::optional<QwenModel> loaded =
        load_qwen_model(temp.path().string(), &error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->forward(ids).data(), expected);  // identical weights + arch
}

TEST(QwenModel, LoadFromDirectoryReportsMissingFilesAndBadCheckpoints) {
    const zetta::test::TempDir temp;
    std::string error;

    // Empty directory: no config.json.
    EXPECT_FALSE(load_qwen_model(temp.path().string(), &error).has_value());
    EXPECT_NE(error.find("config.json"), std::string::npos);

    // config.json present but no model.safetensors.
    std::ofstream(temp.path() / "config.json")
        << R"({"vocab_size":24,"hidden_size":16,"num_hidden_layers":2,)"
           R"("num_attention_heads":4,"num_key_value_heads":2,"head_dim":4,)"
           R"("intermediate_size":32})";
    EXPECT_FALSE(load_qwen_model(temp.path().string(), &error).has_value());
    EXPECT_NE(error.find("model.safetensors"), std::string::npos);

    // A safetensors that doesn't cover the model's parameters.
    SafeTensors sparse;
    sparse["model.norm.weight"] = SafeTensor{{16}, std::vector<float>(16, 1.0F)};
    ASSERT_TRUE(write_safetensors_file((temp.path() / "model.safetensors").string(),
                                       sparse));
    EXPECT_FALSE(load_qwen_model(temp.path().string(), &error).has_value());
    EXPECT_NE(error.find("missing"), std::string::npos);
}

// Loading a checkpoint: dump one model's weights to a .safetensors file, read it
// back, and load it into a FRESH, differently-seeded model. Identical weights =>
// identical logits (F32 is exact). This is exactly how a real Qwen checkpoint
// lands in the model — proven here with a synthetic one (the multi-GB Qwen
// weights can't be fetched in this environment; the format and the load path are
// what matter, and they are the same).
TEST(QwenModel, RoundTripsWeightsThroughASafetensorsFile) {
    const QwenConfig config = tiny_config();
    const QwenModel source(config, 11);
    const std::vector<int> ids = {1, 4, 2, 7, 3};
    const std::vector<float> expected = source.forward(ids).data();

    const zetta::test::TempDir temp;
    const std::string path = (temp.path() / "qwen.safetensors").string();
    ASSERT_TRUE(write_safetensors_file(path, source.to_safetensors()));

    QwenModel loaded(config, 999);  // different seed => different random init
    const std::vector<float> before_load = loaded.forward(ids).data();
    EXPECT_NE(before_load, expected);  // the two models start out different

    const std::optional<SafeTensors> parsed = read_safetensors_file(path);
    ASSERT_TRUE(parsed.has_value());
    QwenModel::LoadReport report;
    ASSERT_TRUE(loaded.load_safetensors(*parsed, &report));
    EXPECT_TRUE(report.missing.empty());
    EXPECT_TRUE(report.mismatched.empty());
    EXPECT_TRUE(report.unexpected.empty());

    EXPECT_EQ(loaded.forward(ids).data(), expected);  // now identical
}

// The load report is honest: a partial / wrong-sized / extra-tensor checkpoint is
// surfaced, not silently accepted.
TEST(QwenModel, LoadReportsMissingMismatchedAndUnexpected) {
    const QwenConfig config = tiny_config();
    QwenModel model(config, 2);

    SafeTensors partial;
    // A correctly-shaped tensor for one real parameter.
    partial["model.norm.weight"] =
        SafeTensor{{config.d_model}, std::vector<float>(config.d_model, 1.0F)};
    // A real parameter name but the wrong element count.
    partial["model.embed_tokens.weight"] = SafeTensor{{1}, {0.0F}};
    // A tensor the model doesn't have.
    partial["model.layers.99.self_attn.q_proj.weight"] = SafeTensor{{1}, {0.0F}};

    QwenModel::LoadReport report;
    EXPECT_FALSE(model.load_safetensors(partial, &report));
    // model.norm.weight loaded; embed_tokens mismatched; many params missing.
    EXPECT_FALSE(report.missing.empty());
    EXPECT_EQ(report.mismatched, (std::vector<std::string>{"model.embed_tokens.weight"}));
    EXPECT_EQ(report.unexpected,
              (std::vector<std::string>{"model.layers.99.self_attn.q_proj.weight"}));
}

// Tied-embedding models round-trip too (no lm_head tensor in the dump).
TEST(QwenModel, TiedEmbeddingsRoundTrip) {
    QwenConfig config = tiny_config();
    config.tie_word_embeddings = true;
    const QwenModel source(config, 4);
    const std::vector<int> ids = {3, 1, 2};
    const std::vector<float> expected = source.forward(ids).data();

    const SafeTensors dump = source.to_safetensors();
    EXPECT_EQ(dump.count("lm_head.weight"), 0U);

    QwenModel loaded(config, 77);
    ASSERT_TRUE(loaded.load_safetensors(dump));
    EXPECT_EQ(loaded.forward(ids).data(), expected);
}

// THE PAYOFF: the full Qwen3-parity stack — GQA + QK-norm + RoPE + SwiGLU +
// RMSNorm, HF-named params — trains on our own autograd. Adam collapses the loss
// on a memorized sequence, and the model then predicts it greedily. Every
// gradient came from our backward closures; there is no framework anywhere.
TEST(QwenModel, OverfitsASequenceEndToEnd) {
    QwenModel model(tiny_config(), 3);
    Adam optimizer(/*learning_rate=*/1e-2F);
    const std::vector<int> sequence = {2, 5, 7, 3, 9, 1, 4, 8};
    const std::vector<int> targets(sequence.begin() + 1, sequence.end());
    const int vocab = model.config().vocab_size;

    float first_loss = 0.0F;
    float last_loss = 0.0F;
    for (int step = 0; step < 400; ++step) {
        zero_grad(model.parameters());
        const Tensor logits = model.forward(sequence);
        const Tensor predicted =
            row_slice(logits, 0, static_cast<int>(sequence.size()) - 1);
        Tensor loss = cross_entropy(predicted, targets);
        loss.backward();
        optimizer.step(model.parameters());
        if (step == 0) first_loss = loss.data()[0];
        last_loss = loss.data()[0];
    }
    EXPECT_LT(last_loss, first_loss * 0.2F);  // the loss collapsed

    const Tensor logits = model.forward(sequence);
    for (std::size_t t = 0; t + 1 < sequence.size(); ++t) {
        const float* row = logits.data().data() + t * vocab;
        int best = 0;
        for (int c = 1; c < vocab; ++c) {
            if (row[c] > row[best]) best = c;
        }
        EXPECT_EQ(best, sequence[t + 1]) << "position " << t;
    }
}

// Train a model to memorize `sequence` (next-token cross-entropy), returning it.
QwenModel overfit_on(const std::vector<int>& sequence, std::uint64_t seed) {
    QwenModel model(tiny_config(), seed);
    Adam optimizer(/*learning_rate=*/1e-2F);
    const std::vector<int> targets(sequence.begin() + 1, sequence.end());
    for (int step = 0; step < 400; ++step) {
        zero_grad(model.parameters());
        const Tensor predicted =
            row_slice(model.forward(sequence), 0, static_cast<int>(sequence.size()) - 1);
        Tensor loss = cross_entropy(predicted, targets);
        loss.backward();
        optimizer.step(model.parameters());
    }
    return model;
}

TEST(QwenModel, GreedyGenerationIsDeterministicAndBounded) {
    const QwenModel model(tiny_config(), 8);
    const std::vector<int> a = model.generate_greedy({1, 2, 3}, 5);
    const std::vector<int> b = model.generate_greedy({1, 2, 3}, 5);
    EXPECT_EQ(a, b);              // deterministic
    EXPECT_EQ(a.size(), 5U);     // exactly max_new_tokens
    for (int token : a) {
        EXPECT_GE(token, 0);
        EXPECT_LT(token, model.config().vocab_size);
    }
}

// Generation is the mirror of training: a model overfit on a sequence greedily
// GENERATES that sequence's continuation from its first token.
TEST(QwenModel, OverfitModelGeneratesTheMemorizedContinuation) {
    const std::vector<int> sequence = {2, 5, 7, 3, 9, 1, 4, 8};
    const QwenModel model = overfit_on(sequence, 3);
    const std::vector<int> continuation =
        model.generate_greedy({sequence.front()},
                              static_cast<int>(sequence.size()) - 1);
    const std::vector<int> expected(sequence.begin() + 1, sequence.end());
    EXPECT_EQ(continuation, expected);
}

TEST(QwenModel, GenerationStopsAtEos) {
    const std::vector<int> sequence = {2, 5, 7, 3, 9, 1, 4, 8};
    const QwenModel model = overfit_on(sequence, 3);
    // The memorized continuation is 5,7,3,9,1,4,8; stop as soon as 3 appears.
    const std::vector<int> out =
        model.generate_greedy({sequence.front()}, 20, /*eos_id=*/3);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.back(), 3);                 // stopped on the eos token
    EXPECT_EQ(out, (std::vector<int>{5, 7, 3}));
}

TEST(QwenModel, SamplingIsReproducibleAndInVocab) {
    const QwenModel model(tiny_config(), 8);
    std::mt19937 rng_a(123);
    std::mt19937 rng_b(123);
    const std::vector<int> a = model.generate({1, 2}, 6, /*temperature=*/1.0F, rng_a);
    const std::vector<int> b = model.generate({1, 2}, 6, /*temperature=*/1.0F, rng_b);
    EXPECT_EQ(a, b);              // same seed => same samples
    EXPECT_EQ(a.size(), 6U);
    for (int token : a) {
        EXPECT_GE(token, 0);
        EXPECT_LT(token, model.config().vocab_size);
    }
}

}  // namespace
