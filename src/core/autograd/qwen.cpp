#include "zetta/autograd/qwen.hpp"

#include <cstddef>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "zetta/autograd/transformer.hpp"

namespace zetta::autograd {

QwenModel::QwenModel(const QwenConfig& config, std::uint64_t seed)
    : config_(config) {
    std::mt19937 rng(static_cast<unsigned>(seed) + 1);
    std::normal_distribution<float> normal(0.0F, 0.02F);
    const int d = config.d_model;
    const int q_dim = config.n_heads * config.head_dim;
    const int kv_dim = config.n_kv_heads * config.head_dim;

    const auto matrix = [&](int rows, int cols) {
        std::vector<float> values(static_cast<std::size_t>(rows) * cols, 0.0F);
        for (float& value : values) {
            value = normal(rng);
        }
        return values;
    };
    const auto ones = [&](int size) { return std::vector<float>(size, 1.0F); };

    add_parameter("model.embed_tokens.weight", matrix(config.vocab_size, d),
                  {config.vocab_size, d});
    for (int layer = 0; layer < config.n_layers; ++layer) {
        const std::string prefix = layer_prefix(layer);
        add_parameter(prefix + "input_layernorm.weight", ones(d), {d});
        // HF Linear stores weight as [out_features, in_features].
        add_parameter(prefix + "self_attn.q_proj.weight", matrix(q_dim, d), {q_dim, d});
        add_parameter(prefix + "self_attn.k_proj.weight", matrix(kv_dim, d), {kv_dim, d});
        add_parameter(prefix + "self_attn.v_proj.weight", matrix(kv_dim, d), {kv_dim, d});
        add_parameter(prefix + "self_attn.o_proj.weight", matrix(d, q_dim), {d, q_dim});
        add_parameter(prefix + "self_attn.q_norm.weight", ones(config.head_dim),
                      {config.head_dim});
        add_parameter(prefix + "self_attn.k_norm.weight", ones(config.head_dim),
                      {config.head_dim});
        add_parameter(prefix + "post_attention_layernorm.weight", ones(d), {d});
        add_parameter(prefix + "mlp.gate_proj.weight", matrix(config.d_ff, d),
                      {config.d_ff, d});
        add_parameter(prefix + "mlp.up_proj.weight", matrix(config.d_ff, d),
                      {config.d_ff, d});
        add_parameter(prefix + "mlp.down_proj.weight", matrix(d, config.d_ff),
                      {d, config.d_ff});
    }
    add_parameter("model.norm.weight", ones(d), {d});
    if (!config.tie_word_embeddings) {
        add_parameter("lm_head.weight", matrix(config.vocab_size, d),
                      {config.vocab_size, d});
    }
}

std::string QwenModel::layer_prefix(int layer) const {
    return "model.layers." + std::to_string(layer) + ".";
}

void QwenModel::add_parameter(const std::string& name, std::vector<float> data,
                              std::vector<int> shape) {
    index_[name] = parameters_.size();
    parameters_.push_back(Tensor::leaf(std::move(data), std::move(shape), true));
}

Tensor QwenModel::weight(const std::string& name) const {
    const auto found = index_.find(name);
    if (found == index_.end()) {
        return Tensor();
    }
    return parameters_[found->second];
}

bool QwenModel::set_weight(const std::string& name, const std::vector<float>& data) {
    const auto found = index_.find(name);
    if (found == index_.end()) {
        return false;
    }
    std::vector<float>& target = parameters_[found->second].node()->data;
    if (target.size() != data.size()) {
        return false;
    }
    target = data;  // copy in place — preserves the node's identity in parameters_
    return true;
}

SafeTensors QwenModel::to_safetensors() const {
    SafeTensors tensors;
    for (const auto& [name, position] : index_) {
        const std::shared_ptr<Node>& node = parameters_[position].node();
        tensors.emplace(name, SafeTensor{node->shape, node->data});
    }
    return tensors;
}

bool QwenModel::load_safetensors(const SafeTensors& tensors, LoadReport* report) {
    bool complete = true;
    for (const auto& [name, position] : index_) {
        const auto found = tensors.find(name);
        if (found == tensors.end()) {
            if (report != nullptr) report->missing.push_back(name);
            complete = false;
            continue;
        }
        if (!set_weight(name, found->second.data)) {  // element-count mismatch
            if (report != nullptr) report->mismatched.push_back(name);
            complete = false;
        }
    }
    if (report != nullptr) {
        for (const auto& [name, tensor] : tensors) {
            if (index_.find(name) == index_.end()) {
                report->unexpected.push_back(name);
            }
        }
    }
    return complete;
}

Tensor QwenModel::forward(const std::vector<int>& ids) const {
    const auto proj = [&](const std::string& name) {
        // HF weight is [out, in]; our matmul(x, M) needs M [in, out].
        return transpose(weight(name));
    };

    Tensor h = embedding(weight("model.embed_tokens.weight"), ids);  // [seq, d_model]

    for (int layer = 0; layer < config_.n_layers; ++layer) {
        const std::string prefix = layer_prefix(layer);
        const Tensor attn_in =
            rms_norm(h, weight(prefix + "input_layernorm.weight"), config_.rms_eps);
        const Tensor attn = grouped_query_attention(
            attn_in, proj(prefix + "self_attn.q_proj.weight"),
            proj(prefix + "self_attn.k_proj.weight"),
            proj(prefix + "self_attn.v_proj.weight"),
            proj(prefix + "self_attn.o_proj.weight"),
            weight(prefix + "self_attn.q_norm.weight"),
            weight(prefix + "self_attn.k_norm.weight"), config_.n_heads,
            config_.n_kv_heads, config_.head_dim, config_.rope_base);
        h = add(h, attn);

        const Tensor mlp_in = rms_norm(
            h, weight(prefix + "post_attention_layernorm.weight"), config_.rms_eps);
        const Tensor mlp = swiglu_feed_forward(
            mlp_in, proj(prefix + "mlp.gate_proj.weight"),
            proj(prefix + "mlp.up_proj.weight"), proj(prefix + "mlp.down_proj.weight"));
        h = add(h, mlp);
    }

    h = rms_norm(h, weight("model.norm.weight"), config_.rms_eps);
    const std::string output_name =
        config_.tie_word_embeddings ? "model.embed_tokens.weight" : "lm_head.weight";
    return matmul(h, transpose(weight(output_name)));  // logits [seq, vocab]
}

}  // namespace zetta::autograd
