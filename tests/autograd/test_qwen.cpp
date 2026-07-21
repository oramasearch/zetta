#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"

namespace {

using zetta::autograd::QwenConfig;
using zetta::autograd::QwenModel;
using zetta::autograd::Tensor;

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

}  // namespace
