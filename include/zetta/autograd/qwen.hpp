#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "zetta/autograd/safetensors.hpp"
#include "zetta/autograd/tensor.hpp"

// A Qwen3-parity decoder-only transformer on zetta's own autograd (#555;
// docs/autograd/README.md). It assembles the pieces we built and gradient-checked
// — RMSNorm, grouped-query attention with QK-norm + RoPE, and the SwiGLU MLP —
// into the exact block structure and tensor names Qwen3 uses on the Hugging Face
// hub, so a real safetensors checkpoint loads into it by name (the loader lands in
// a later slice). Parameters are autograd leaves, so the same model trains (overfit
// / RLVR) and runs inference with no framework.
namespace zetta::autograd {

class Adam;

// The architecture hyperparameters. Defaults are a tiny test-sized model; a real
// Qwen3 checkpoint sets them from its config.json.
struct QwenConfig {
    int vocab_size = 32;
    int d_model = 16;                 // hidden_size
    int n_layers = 2;
    int n_heads = 4;                  // query heads
    int n_kv_heads = 2;               // key/value heads (GQA); == n_heads is MHA
    int head_dim = 4;                 // per-head width (Qwen3 may decouple from d_model)
    int d_ff = 32;                    // intermediate_size (the SwiGLU width)
    float rms_eps = 1e-6F;            // RMSNorm epsilon
    float rope_base = 1000000.0F;     // RoPE θ (Qwen3 default)
    bool tie_word_embeddings = false; // reuse embed_tokens as the output projection
};

// Build a QwenConfig from a Hugging Face `config.json` body — the fields Qwen3
// carries: vocab_size, hidden_size, num_hidden_layers, num_attention_heads,
// num_key_value_heads, head_dim (falls back to hidden_size / num_attention_heads
// if absent), intermediate_size, rms_norm_eps, rope_theta, tie_word_embeddings.
// A non-throwing boundary: std::nullopt (with a reason in *error) if the JSON is
// invalid, a required field is missing, or the head counts are inconsistent
// (n_heads % n_kv_heads != 0). This is how the right architecture is instantiated
// from a checkpoint before its weights load.
std::optional<QwenConfig> parse_qwen_config(std::string_view json,
                                            std::string* error = nullptr);

// Hugging Face tensor names for this architecture (what a Qwen3 safetensors holds):
//   model.embed_tokens.weight                              [vocab, d_model]
//   model.layers.{L}.input_layernorm.weight               [d_model]
//   model.layers.{L}.self_attn.{q,k,v,o}_proj.weight      [out, in]  (HF Linear layout)
//   model.layers.{L}.self_attn.{q,k}_norm.weight          [head_dim]
//   model.layers.{L}.post_attention_layernorm.weight      [d_model]
//   model.layers.{L}.mlp.{gate,up,down}_proj.weight       [out, in]
//   model.norm.weight                                     [d_model]
//   lm_head.weight                                        [vocab, d_model] (absent if tied)
class QwenModel {
public:
    // Randomly initializes every parameter (matrices ~N(0, 0.02), norms = ones) at
    // the shapes this config implies, registered under their HF names.
    explicit QwenModel(const QwenConfig& config, std::uint64_t seed = 0);

    const QwenConfig& config() const { return config_; }

    // Training view: every parameter as an autograd leaf, in a stable order (so an
    // optimizer's per-parameter moments line up across steps).
    std::vector<Tensor>& parameters() { return parameters_; }
    const std::vector<Tensor>& parameters() const { return parameters_; }

    std::size_t resident_bytes() const;

    // Look up a parameter by its HF name. Returns an invalid Tensor if absent.
    Tensor weight(const std::string& name) const;

    // Load a checkpoint tensor by HF name: copies `data` into the existing
    // parameter's node (preserving its identity in parameters()). Returns false if
    // the name is unknown or the element count does not match the config's shape —
    // a non-throwing boundary (the caller reports the path-qualified reason).
    bool set_weight(const std::string& name, const std::vector<float>& data);

    // Dump every parameter as an HF-name -> tensor map (to save a trained model or
    // to round-trip through a .safetensors file).
    SafeTensors to_safetensors() const;

    // What a checkpoint load did not line up cleanly.
    struct LoadReport {
        std::vector<std::string> missing;      // model params absent from the file
        std::vector<std::string> mismatched;   // name matched but element count differed
        std::vector<std::string> unexpected;   // file tensors with no matching param
    };

    // Load parameters from a safetensors map by HF name (via set_weight, copying in
    // place). Returns true iff every model parameter was present with a matching
    // element count; extra tensors in the file are recorded in report->unexpected
    // but do not fail the load. This is how a real Qwen checkpoint lands in the model.
    bool load_safetensors(const SafeTensors& tensors, LoadReport* report = nullptr);

    // Logits [seq, vocab] for the token id sequence, through the full Qwen3 stack.
    Tensor forward(const std::vector<int>& ids) const;

    // Autoregressive greedy decode: append argmax(last-position logits) to the
    // running sequence up to `max_new_tokens`, stopping early if `eos_id` (>= 0) is
    // produced. Returns the generated tokens only (not the prompt). Deterministic.
    // Re-forwards the whole sequence each step (no KV cache — a later optimization).
    std::vector<int> generate_greedy(const std::vector<int>& prompt,
                                     int max_new_tokens, int eos_id = -1) const;

    // Autoregressive sampling: each next token is drawn from
    // softmax(last-position logits / temperature) with `rng`. temperature <= 0
    // falls back to greedy. This is the "act" step of the RLVR loop.
    std::vector<int> generate(const std::vector<int>& prompt, int max_new_tokens,
                              float temperature, std::mt19937& rng,
                              int eos_id = -1) const;

private:
    std::string layer_prefix(int layer) const;
    void add_parameter(const std::string& name, std::vector<float> data,
                       std::vector<int> shape);
    std::vector<int> generate_impl(const std::vector<int>& prompt, int max_new_tokens,
                                   float temperature, std::mt19937* rng,
                                   int eos_id) const;

    QwenConfig config_;
    std::vector<Tensor> parameters_;                     // canonical, ordered
    std::unordered_map<std::string, std::size_t> index_; // HF name -> position
};

struct QwenFineTuneMetrics {
    double loss = 0.0;
    double learning_rate = 0.0;
    double gradient_norm = 0.0;
    double tokens_per_second = 0.0;

    std::map<std::string, double> values() const;
};

using QwenMetricLogger =
    std::function<bool(std::uint64_t, const std::map<std::string, double>&)>;

// Perform one completion-masked next-token fine-tuning step and report the
// standard trainer metrics through the supplied Run logger.
std::optional<QwenFineTuneMetrics> fine_tune_qwen_step(
    QwenModel& model, Adam& optimizer, const std::vector<int>& ids,
    int first_scored, std::uint64_t step,
    const QwenMetricLogger& logger = {}, std::string* error = nullptr);

// Load a Qwen model from a Hugging Face checkpoint directory: read
// `<directory>/config.json` for the architecture (parse_qwen_config) and
// `<directory>/model.safetensors` for the weights, build the QwenModel, and load
// every parameter by name. Returns std::nullopt (with a reason in *error) if
// either file is missing/malformed, or the checkpoint does not cover the model's
// parameters (missing / mismatched tensors). THE entry point: point it at a
// downloaded Qwen directory and get a trainable model. (Single-file
// model.safetensors; sharded index.json is a later slice.)
std::optional<QwenModel> load_qwen_model(const std::string& directory,
                                         std::string* error = nullptr);

// Fast KV-cached autoregressive inference for a QwenModel. Generation needs no
// gradients, so this is a plain-float forward (no autograd graph) that forwards ONE
// token at a time: each new token's attention reads the cached key/value of all
// prior tokens, so a step is O(sequence) instead of re-forwarding the whole
// sequence through every layer (the O(n²) cost of QwenModel::generate). It produces
// the same logits as QwenModel::forward. Weights are read from the model in Hugging
// Face layout and applied as x·Wᵀ; the lm_head projection spreads over the runtime
// thread pool. Not differentiable — for generation / RLVR rollouts, not training.
class QwenInference {
public:
    explicit QwenInference(const QwenModel& model);

    // Clear the KV cache and reset to position 0.
    void reset();

    // Forward one token at the current position: append its key/value to the cache
    // and return its logits [vocab]. Advances the position by one.
    std::vector<float> step(int token_id);

    // Greedy decode: reset, run the prompt through the cache, then repeatedly step
    // on the argmax until `max_new_tokens` or `eos_id` (>= 0). Returns the generated
    // tokens (not the prompt) — same result as QwenModel::generate_greedy, faster.
    std::vector<int> generate_greedy(const std::vector<int>& prompt,
                                     int max_new_tokens, int eos_id = -1);

    // Sampling decode through the cache: each next token drawn from
    // softmax(logits / temperature) with `rng` (temperature <= 0 is greedy). The
    // fast rollout path for RLVR — the same distribution as QwenModel::generate.
    std::vector<int> generate(const std::vector<int>& prompt, int max_new_tokens,
                              float temperature, std::mt19937& rng, int eos_id = -1);

private:
    const QwenModel& model_;
    QwenConfig config_;
    int position_ = 0;
    std::vector<std::vector<float>> key_cache_;    // per layer, flat [pos][n_kv*head_dim]
    std::vector<std::vector<float>> value_cache_;
};

// Serialize a config to a Hugging Face `config.json` body (the fields
// parse_qwen_config reads, so config -> json -> config round-trips).
std::string serialize_qwen_config(const QwenConfig& config);

// Save a model to a Hugging Face checkpoint directory: write
// `<directory>/config.json` and `<directory>/model.safetensors` (single file),
// creating the directory if needed. Returns false (with a reason in *error) if the
// directory can't be created or a file can't be written. The inverse of
// load_qwen_model — how a post-trained base-agent checkpoint is shipped (load a
// base -> RLVR post-train -> save the improved checkpoint).
//
// `source_directory` is the checkpoint the weights were loaded from; when given, its TOKENIZER
// files are copied alongside the weights so the result is a checkpoint that loads on its own.
// Without them the directory holds weights that nothing can tokenize for, and every consumer has
// to know to go find the base model and copy three files by hand — which has silently produced an
// unloadable checkpoint more than once. Copy, don't reference: a checkpoint that depends on a path
// on the training machine is not a checkpoint you can ship.
bool save_qwen_model(const QwenModel& model, const std::string& directory,
                     std::string* error = nullptr,
                     const std::string& source_directory = {});

}  // namespace zetta::autograd
