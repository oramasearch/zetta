#include "zetta/agent/qwen_rlvr.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "zetta/agent/advantage.hpp"
#include "zetta/autograd/tensor.hpp"

namespace zetta::agent {

double grpo_step(autograd::QwenModel& model, autograd::Adam& optimizer,
                 const std::vector<int>& prompt, const CompletionReward& reward,
                 const GrpoConfig& config, std::mt19937& rng) {
    const int prompt_len = static_cast<int>(prompt.size());

    // Roll out the group and score each completion.
    std::vector<std::vector<int>> completions;
    std::vector<double> rewards;
    completions.reserve(static_cast<std::size_t>(config.group_size));
    rewards.reserve(static_cast<std::size_t>(config.group_size));
    double reward_sum = 0.0;
    for (int i = 0; i < config.group_size; ++i) {
        std::vector<int> completion =
            model.generate(prompt, config.max_new_tokens, config.temperature, rng);
        const double value = reward(prompt, completion);
        reward_sum += value;
        completions.push_back(std::move(completion));
        rewards.push_back(value);
    }
    const std::vector<double> advantages = group_advantages(rewards);

    // Sequence policy-gradient update, averaged over the completions that produced
    // at least one token. Each completion's scalar advantage is applied to every
    // token it emitted (trajectory-level credit — the GRPO shape).
    autograd::zero_grad(model.parameters());
    autograd::Tensor total;
    int contributing = 0;
    for (int i = 0; i < config.group_size; ++i) {
        const std::vector<int>& completion = completions[i];
        const int generated = static_cast<int>(completion.size());
        if (generated == 0) {
            continue;
        }
        std::vector<int> full = prompt;
        full.insert(full.end(), completion.begin(), completion.end());

        // forward([prompt+completion]) row r predicts token r+1, so the rows that
        // produced the completion tokens are [prompt_len-1 .. prompt_len+gen-2].
        const autograd::Tensor logits = model.forward(full);
        const autograd::Tensor aligned =
            autograd::row_slice(logits, prompt_len - 1, generated);
        autograd::Tensor loss =
            autograd::sequence_policy_gradient_loss(aligned, completion, advantages[i]);
        total = total.valid() ? autograd::add(total, loss) : loss;
        ++contributing;
    }

    if (contributing > 0) {
        autograd::Tensor mean_loss =
            autograd::scale(total, 1.0F / static_cast<float>(contributing));
        mean_loss.backward();
        optimizer.step(model.parameters());
    }

    return config.group_size > 0 ? reward_sum / static_cast<double>(config.group_size)
                                 : 0.0;
}

}  // namespace zetta::agent
