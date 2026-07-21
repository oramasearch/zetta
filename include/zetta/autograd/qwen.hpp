#pragma once

#include <cstdint>
#include <string>
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

private:
    std::string layer_prefix(int layer) const;
    void add_parameter(const std::string& name, std::vector<float> data,
                       std::vector<int> shape);

    QwenConfig config_;
    std::vector<Tensor> parameters_;                     // canonical, ordered
    std::unordered_map<std::string, std::size_t> index_; // HF name -> position
};

}  // namespace zetta::autograd
