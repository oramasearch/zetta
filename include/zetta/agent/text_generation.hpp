#pragma once

#include <random>
#include <string>

#include "zetta/autograd/qwen.hpp"
#include "zetta/tokenizer/bpe.hpp"

// Text in, text out (#555; docs/agent/): compose the byte-level BPE tokenizer with
// the Qwen model so the agent works in natural language, not token ids. Encode the
// prompt, autoregressively decode new tokens, and turn just the new tokens back
// into text. This is the surface a real Qwen checkpoint + its tokenizer present
// once both are loaded (load_qwen_model + BpeTokenizer::from_directory).
namespace zetta::agent {

// Greedy: encode `prompt`, generate up to `max_new_tokens` (stopping early on
// `eos_id` >= 0), and return the decoded completion (the NEW tokens only, not the
// prompt). Deterministic.
std::string generate_text(const autograd::QwenModel& model,
                          const tokenizer::BpeTokenizer& tokenizer,
                          const std::string& prompt, int max_new_tokens,
                          int eos_id = -1);

// Sampling variant: each next token drawn from softmax(logits / temperature) with
// `rng`. temperature <= 0 falls back to greedy.
std::string generate_text(const autograd::QwenModel& model,
                          const tokenizer::BpeTokenizer& tokenizer,
                          const std::string& prompt, int max_new_tokens,
                          float temperature, std::mt19937& rng, int eos_id = -1);

}  // namespace zetta::agent
