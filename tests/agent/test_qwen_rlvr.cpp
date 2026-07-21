#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <vector>

#include "zetta/agent/qwen_rlvr.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/transformer.hpp"

namespace {

using zetta::agent::CompletionReward;
using zetta::agent::grpo_step;
using zetta::agent::GrpoConfig;
using zetta::autograd::Adam;
using zetta::autograd::QwenConfig;
using zetta::autograd::QwenModel;

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

// THE CAPSTONE (#555): the open-weights Qwen3-parity model — GQA + QK-norm + RoPE
// + SwiGLU + RMSNorm, on zetta's own autograd — is RLVR post-trained PURELY FROM A
// VERIFIABLE REWARD, never shown the answer. This is blender-rl's GRPO loop
// reproduced native and device-agnostic: sample a group of completions, score
// each, normalize to group-relative advantages, take a sequence policy-gradient
// step. The model learns to generate the target sequence from reward alone.
TEST(QwenRlvr, LearnsAVerifiableSequenceByGrpo) {
    QwenModel model(tiny_config(), 3);
    Adam optimizer(/*learning_rate=*/1e-2F);

    const std::vector<int> prompt = {1};
    const std::vector<int> target = {5, 8};  // the model must learn to emit this
    // Verifiable reward: fraction of positions that match the target (partial
    // credit) — a check that runs, not a learned reward model.
    const CompletionReward reward = [&](const std::vector<int>&,
                                        const std::vector<int>& completion) {
        double correct = 0.0;
        for (std::size_t t = 0; t < target.size() && t < completion.size(); ++t) {
            if (completion[t] == target[t]) correct += 1.0;
        }
        return correct / static_cast<double>(target.size());
    };

    GrpoConfig config;
    config.group_size = 32;
    config.max_new_tokens = static_cast<int>(target.size());
    config.temperature = 1.0F;

    // It does not already know the answer.
    EXPECT_NE(model.generate_greedy(prompt, config.max_new_tokens), target);

    std::mt19937 rng(7);
    double first_reward = 0.0;
    double last_reward = 0.0;
    bool converged = false;
    for (int step = 0; step < 500; ++step) {
        const double mean_reward = grpo_step(model, optimizer, prompt, reward, config, rng);
        if (step == 0) first_reward = mean_reward;
        last_reward = mean_reward;
        if (step % 10 == 0 &&
            model.generate_greedy(prompt, config.max_new_tokens) == target) {
            converged = true;
            break;
        }
    }

    EXPECT_TRUE(converged) << "greedy generation never reached the target";
    EXPECT_EQ(model.generate_greedy(prompt, config.max_new_tokens), target);
    EXPECT_GT(last_reward, first_reward);  // reward rose over training
}

}  // namespace
