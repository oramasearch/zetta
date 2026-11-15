// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <random>
#include <string>
#include <vector>

#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/transformer.hpp"  // Adam, zero_grad
#include "zetta/tokenizer/bpe.hpp"

// RLVR post-training of the open-weights Qwen model on zetta's own autograd (#555;
// docs/agent/README.md). This is blender-rl's GRPO loop reproduced native and
// device-agnostic: roll out a GROUP of completions from the model, score each with
// a VERIFIABLE reward, turn the group's rewards into group-relative advantages
// (the group is its own baseline — no value function), and take one sequence
// policy-gradient step. The model is the autograd `QwenModel`, so the same loop
// runs on CPU / CUDA / MLX (the ComputeDevice the graph is built on) — one
// distributed model can train across the cluster. No framework: every gradient
// comes from the autograd's backward closures.
namespace zetta::agent {

// A verifiable reward: given the prompt and one sampled completion (token ids),
// return a real-valued reward (e.g. 1 if correct, a fraction for partial credit).
// It is a check that runs — not a learned reward model.
using CompletionReward = std::function<double(const std::vector<int>& prompt,
                                              const std::vector<int>& completion)>;

struct GrpoConfig {
    int group_size = 16;       // completions rolled out per prompt (the group)
    int max_new_tokens = 1;    // completion length
    float temperature = 1.0F;  // rollout sampling temperature
};

// One GRPO/RLVR update over a QwenModel for a single (non-empty) prompt: sample
// `group_size` completions, score each with `reward`, normalize the rewards into
// group-relative advantages, and take a sequence policy-gradient step averaged
// over the group with `optimizer`. Returns the group's mean reward — the learning
// signal to watch rise. The rollout randomness is injected via `rng` so runs
// reproduce.
double grpo_step(autograd::QwenModel& model, autograd::Adam& optimizer,
                 const std::vector<int>& prompt, const CompletionReward& reward,
                 const GrpoConfig& config, std::mt19937& rng);

// A verifiable reward over TEXT: given the prompt and one decoded completion,
// return a real-valued reward.
using TextCompletionReward =
    std::function<double(const std::string& prompt, const std::string& completion)>;

// The text-level GRPO/RLVR step: encode `prompt` with `tokenizer`, run one
// grpo_step, and score each rollout by DECODING its completion tokens back to text
// and passing that to `reward`. This is RLVR post-training in natural language —
// the model learns to produce target text purely from a verifiable text reward,
// composing the tokenizer with the token-level loop.
double grpo_step_text(autograd::QwenModel& model, autograd::Adam& optimizer,
                      const tokenizer::BpeTokenizer& tokenizer,
                      const std::string& prompt, const TextCompletionReward& reward,
                      const GrpoConfig& config, std::mt19937& rng);

}  // namespace zetta::agent
