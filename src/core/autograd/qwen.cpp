#include "zetta/autograd/qwen.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/autograd/transformer.hpp"

namespace zetta::autograd {

namespace {

// Load a sharded checkpoint: read a `model.safetensors.index.json` (its
// `weight_map` maps each tensor name to the shard file that holds it), read each
// referenced shard from the same directory, and merge them into one map. Returns
// std::nullopt (with a reason) on a bad index or an unreadable/malformed shard.
std::optional<SafeTensors> read_sharded_safetensors(
    const std::filesystem::path& root, const std::filesystem::path& index_path,
    std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<SafeTensors> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };

    std::ifstream in(index_path, std::ios::binary);
    if (!in) {
        return fail("load_qwen: cannot open " + index_path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const nlohmann::json index =
        nlohmann::json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (index.is_discarded() || !index.is_object() ||
        !index.contains("weight_map") || !index.at("weight_map").is_object()) {
        return fail("load_qwen: " + index_path.string() +
                    " has no object 'weight_map'");
    }

    // The distinct shard files the weight_map references.
    std::set<std::string> shard_files;
    for (const auto& [name, file] : index.at("weight_map").items()) {
        if (!file.is_string()) {
            return fail("load_qwen: weight_map entry '" + name + "' is not a string");
        }
        shard_files.insert(file.get<std::string>());
    }

    SafeTensors merged;
    for (const std::string& shard : shard_files) {
        const std::optional<SafeTensors> part =
            read_safetensors_file((root / shard).string(), error);
        if (!part.has_value()) {
            return std::nullopt;  // *error already set by read_safetensors_file
        }
        for (const auto& [name, tensor] : *part) {
            merged.emplace(name, tensor);
        }
    }
    return merged;
}

}  // namespace

std::optional<QwenConfig> parse_qwen_config(std::string_view json,
                                            std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<QwenConfig> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };

    const nlohmann::json root =
        nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        return fail("qwen config: not a valid JSON object");
    }

    // Every field a required integer, read non-throwing (sets *error and returns
    // false on a missing / non-integer field).
    bool ok = true;
    const auto require_int = [&](const char* key, int* out) {
        if (!ok) return;  // first failure wins
        if (!root.contains(key) || !root.at(key).is_number_integer()) {
            ok = false;
            if (error != nullptr) {
                *error = std::string("qwen config: missing/non-integer '") + key + "'";
            }
            return;
        }
        *out = root.at(key).get<int>();
    };

    QwenConfig config;
    require_int("vocab_size", &config.vocab_size);
    require_int("hidden_size", &config.d_model);
    require_int("num_hidden_layers", &config.n_layers);
    require_int("num_attention_heads", &config.n_heads);
    require_int("num_key_value_heads", &config.n_kv_heads);
    require_int("intermediate_size", &config.d_ff);
    if (!ok) {
        return std::nullopt;  // *error already set by require_int
    }

    // head_dim is explicit in Qwen3, but fall back to hidden_size/num_heads.
    if (root.contains("head_dim") && root.at("head_dim").is_number_integer()) {
        config.head_dim = root.at("head_dim").get<int>();
    } else if (config.n_heads > 0) {
        config.head_dim = config.d_model / config.n_heads;
    }

    // Optional floats/bools with sensible Qwen3 defaults.
    if (root.contains("rms_norm_eps") && root.at("rms_norm_eps").is_number()) {
        config.rms_eps = root.at("rms_norm_eps").get<float>();
    }
    if (root.contains("rope_theta") && root.at("rope_theta").is_number()) {
        config.rope_base = root.at("rope_theta").get<float>();
    }
    if (root.contains("tie_word_embeddings") && root.at("tie_word_embeddings").is_boolean()) {
        config.tie_word_embeddings = root.at("tie_word_embeddings").get<bool>();
    }

    if (config.vocab_size <= 0 || config.d_model <= 0 || config.n_layers <= 0 ||
        config.n_heads <= 0 || config.n_kv_heads <= 0 || config.head_dim <= 0 ||
        config.d_ff <= 0) {
        return fail("qwen config: dimensions must be positive");
    }
    if (config.n_heads % config.n_kv_heads != 0) {
        return fail("qwen config: num_attention_heads must be a multiple of "
                    "num_key_value_heads");
    }
    return config;
}

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

std::vector<int> QwenModel::generate_impl(const std::vector<int>& prompt,
                                          int max_new_tokens, float temperature,
                                          std::mt19937* rng, int eos_id) const {
    const int vocab = config_.vocab_size;
    std::vector<int> running = prompt;
    std::vector<int> generated;
    generated.reserve(static_cast<std::size_t>(max_new_tokens));

    for (int produced = 0; produced < max_new_tokens; ++produced) {
        const Tensor logits = forward(running);  // [seq, vocab]
        const std::size_t last = running.size() - 1;
        const float* row = logits.data().data() + last * vocab;

        int next = 0;
        if (temperature <= 0.0F || rng == nullptr) {
            for (int c = 1; c < vocab; ++c) {
                if (row[c] > row[next]) next = c;  // greedy argmax
            }
        } else {
            // Sample from softmax(row / temperature), numerically stable.
            float max_logit = row[0];
            for (int c = 1; c < vocab; ++c) max_logit = std::max(max_logit, row[c]);
            std::vector<double> probs(static_cast<std::size_t>(vocab), 0.0);
            double sum_exp = 0.0;
            for (int c = 0; c < vocab; ++c) {
                probs[c] = std::exp((row[c] - max_logit) / temperature);
                sum_exp += probs[c];
            }
            std::uniform_real_distribution<double> uniform(0.0, sum_exp);
            const double threshold = uniform(*rng);
            double cumulative = 0.0;
            next = vocab - 1;
            for (int c = 0; c < vocab; ++c) {
                cumulative += probs[c];
                if (threshold < cumulative) {
                    next = c;
                    break;
                }
            }
        }

        generated.push_back(next);
        running.push_back(next);
        if (eos_id >= 0 && next == eos_id) {
            break;
        }
    }
    return generated;
}

std::vector<int> QwenModel::generate_greedy(const std::vector<int>& prompt,
                                            int max_new_tokens, int eos_id) const {
    return generate_impl(prompt, max_new_tokens, /*temperature=*/0.0F,
                         /*rng=*/nullptr, eos_id);
}

std::vector<int> QwenModel::generate(const std::vector<int>& prompt,
                                     int max_new_tokens, float temperature,
                                     std::mt19937& rng, int eos_id) const {
    return generate_impl(prompt, max_new_tokens, temperature, &rng, eos_id);
}

std::optional<QwenModel> load_qwen_model(const std::string& directory,
                                         std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<QwenModel> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };

    const std::filesystem::path root(directory);

    // config.json -> the architecture.
    std::ifstream config_in(root / "config.json", std::ios::binary);
    if (!config_in) {
        return fail("load_qwen: cannot open " + (root / "config.json").string());
    }
    std::ostringstream config_buffer;
    config_buffer << config_in.rdbuf();
    std::string reason;
    const std::optional<QwenConfig> config =
        parse_qwen_config(config_buffer.str(), &reason);
    if (!config.has_value()) {
        return fail(reason);
    }

    // The weights: a single model.safetensors, or a sharded set indexed by
    // model.safetensors.index.json (how real multi-GB Qwen checkpoints ship).
    std::optional<SafeTensors> tensors;
    const std::filesystem::path single = root / "model.safetensors";
    const std::filesystem::path index = root / "model.safetensors.index.json";
    if (std::filesystem::exists(single)) {
        tensors = read_safetensors_file(single.string(), &reason);
    } else if (std::filesystem::exists(index)) {
        tensors = read_sharded_safetensors(root, index, &reason);
    } else {
        return fail("load_qwen: no model.safetensors or model.safetensors.index.json "
                    "in " + directory);
    }
    if (!tensors.has_value()) {
        return fail(reason);
    }

    // Build the model and load every parameter by name.
    QwenModel model(*config);
    QwenModel::LoadReport report;
    if (!model.load_safetensors(*tensors, &report)) {
        std::string detail = "load_qwen: checkpoint does not cover the model —";
        if (!report.missing.empty()) {
            detail += " " + std::to_string(report.missing.size()) +
                      " missing (e.g. " + report.missing.front() + ")";
        }
        if (!report.mismatched.empty()) {
            detail += " " + std::to_string(report.mismatched.size()) +
                      " mismatched (e.g. " + report.mismatched.front() + ")";
        }
        return fail(detail);
    }
    return model;
}

}  // namespace zetta::autograd
