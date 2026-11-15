// SPDX-License-Identifier: MIT

#include "zetta/agent/text_generation.hpp"

#include <vector>

namespace zetta::agent {

std::string generate_text(const autograd::QwenModel& model,
                          const tokenizer::BpeTokenizer& tokenizer,
                          const std::string& prompt, int max_new_tokens,
                          int eos_id) {
    const std::vector<int> prompt_ids = tokenizer.encode(prompt);
    const std::vector<int> completion =
        model.generate_greedy(prompt_ids, max_new_tokens, eos_id);
    return tokenizer.decode(completion);
}

std::string generate_text(const autograd::QwenModel& model,
                          const tokenizer::BpeTokenizer& tokenizer,
                          const std::string& prompt, int max_new_tokens,
                          float temperature, std::mt19937& rng, int eos_id) {
    const std::vector<int> prompt_ids = tokenizer.encode(prompt);
    const std::vector<int> completion =
        model.generate(prompt_ids, max_new_tokens, temperature, rng, eos_id);
    return tokenizer.decode(completion);
}

}  // namespace zetta::agent
