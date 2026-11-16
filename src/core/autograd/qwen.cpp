// SPDX-License-Identifier: MIT

#include "zetta/autograd/qwen.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/autograd/transformer.hpp"
#include "zetta/runtime.hpp"

namespace zetta::autograd {

std::size_t QwenModel::resident_bytes() const {
    std::size_t bytes = sizeof(QwenModel);
    std::unordered_set<const Node*> seen;
    for (const auto& parameter : parameters_) {
        if (!parameter.valid() || !seen.insert(parameter.node().get()).second) {
            continue;
        }
        const auto& node = *parameter.node();
        bytes += sizeof(Node);
        bytes += node.data.capacity() * sizeof(float);
        bytes += node.grad.capacity() * sizeof(float);
        bytes += node.shape.capacity() * sizeof(int);
        bytes += node.parents.capacity() * sizeof(std::shared_ptr<Node>);
    }
    for (const auto& [name, index] : index_) {
        (void)index;
        bytes += sizeof(std::pair<const std::string, std::size_t>) +
                 name.capacity();
    }
    return bytes;
}

namespace {

// out[o] = dot(W_row_o, x), W is [out, in] (Hugging Face layout). Spread over the
// output rows on the runtime thread pool (threads when out is large, e.g. lm_head).
std::vector<float> matvec_transposed(const std::vector<float>& weight,
                                     const std::vector<float>& x, int out_dim,
                                     int in_dim) {
    std::vector<float> out(static_cast<std::size_t>(out_dim), 0.0F);
    const float* w = weight.data();
    const float* xd = x.data();
    float* od = out.data();
    // Each output row is a length-`in_dim` dot product (heavy) — thread when the
    // total work justifies it, else run inline (grain == count).
    const std::size_t total = static_cast<std::size_t>(out_dim) * in_dim;
    const std::size_t grain = total < (1u << 17) ? static_cast<std::size_t>(out_dim) : 1;
    runtime::parallel_for(
        static_cast<std::size_t>(out_dim), grain,
        [&](std::size_t, std::size_t begin, std::size_t end) {
            for (std::size_t o = begin; o < end; ++o) {
                const float* w_row = w + o * in_dim;
                float acc = 0.0F;
                for (int i = 0; i < in_dim; ++i) acc += w_row[i] * xd[i];
                od[o] = acc;
            }
        });
    return out;
}

// RMSNorm a vector in place: x[i] = x[i]/sqrt(mean(x²)+eps) * weight[i].
void rms_norm_inplace(std::vector<float>& x, const std::vector<float>& weight,
                      float eps) {
    double sum_sq = 0.0;
    for (float v : x) sum_sq += static_cast<double>(v) * v;
    const float inv = 1.0F / std::sqrt(static_cast<float>(sum_sq / x.size()) + eps);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = x[i] * inv * weight[i];
}

// Rotate one head vector [head_dim] by RoPE at `position` (rotate_half convention).
void rope_inplace(float* head, int head_dim, int position, float base) {
    const int half = head_dim / 2;
    for (int i = 0; i < half; ++i) {
        const double inv_freq = std::pow(static_cast<double>(base), -2.0 * i / head_dim);
        const double angle = position * inv_freq;
        const float c = static_cast<float>(std::cos(angle));
        const float s = static_cast<float>(std::sin(angle));
        const float a = head[i];
        const float b = head[i + half];
        head[i] = a * c - b * s;
        head[i + half] = b * c + a * s;
    }
}

}  // namespace

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

std::map<std::string, double> QwenFineTuneMetrics::values() const {
    return {{"train.loss", loss},
            {"train.learning_rate", learning_rate},
            {"train.gradient_norm", gradient_norm},
            {"train.tokens_per_second", tokens_per_second}};
}

std::optional<QwenFineTuneMetrics> fine_tune_qwen_step(
    QwenModel& model, Adam& optimizer, const std::vector<int>& ids,
    int first_scored, std::uint64_t step, const QwenMetricLogger& logger,
    std::string* error) {
    if (ids.size() < 2) {
        if (error != nullptr) {
            *error = "fine_tune_qwen_step: at least two tokens are required";
        }
        return std::nullopt;
    }
    const int predicted_rows = static_cast<int>(ids.size()) - 1;
    if (first_scored < 0 || first_scored >= predicted_rows) {
        if (error != nullptr) {
            *error = "fine_tune_qwen_step: first_scored is out of range";
        }
        return std::nullopt;
    }

    const auto started = std::chrono::steady_clock::now();
    zero_grad(model.parameters());
    const Tensor predicted = row_slice(model.forward(ids), 0, predicted_rows);
    const std::vector<int> targets(ids.begin() + 1, ids.end());
    Tensor loss = cross_entropy(predicted, targets, first_scored);
    loss.backward();

    double gradient_squared = 0.0;
    for (const Tensor& parameter : model.parameters()) {
        for (float gradient : parameter.grad()) {
            gradient_squared += static_cast<double>(gradient) * gradient;
        }
    }
    optimizer.step(model.parameters());

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    QwenFineTuneMetrics metrics;
    metrics.loss = loss.data()[0];
    metrics.learning_rate = optimizer.learning_rate();
    metrics.gradient_norm = std::sqrt(gradient_squared);
    metrics.tokens_per_second =
        static_cast<double>(predicted_rows - first_scored) /
        std::max(elapsed, std::numeric_limits<double>::min());
    if (logger && !logger(step, metrics.values())) {
        if (error != nullptr) {
            *error = "fine_tune_qwen_step: Run metric logging failed";
        }
        return std::nullopt;
    }
    return metrics;
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
    // Weights stay in Hugging Face layout [out, in]; the attention/MLP blocks and
    // the lm_head apply them via matmul_transposed_b (x·Wᵀ), so no weight is ever
    // transposed on the forward — the dominant cost of running a real checkpoint.
    Tensor h = embedding(weight("model.embed_tokens.weight"), ids);  // [seq, d_model]

    for (int layer = 0; layer < config_.n_layers; ++layer) {
        const std::string prefix = layer_prefix(layer);
        const Tensor attn_in =
            rms_norm(h, weight(prefix + "input_layernorm.weight"), config_.rms_eps);
        const Tensor attn = grouped_query_attention(
            attn_in, weight(prefix + "self_attn.q_proj.weight"),
            weight(prefix + "self_attn.k_proj.weight"),
            weight(prefix + "self_attn.v_proj.weight"),
            weight(prefix + "self_attn.o_proj.weight"),
            weight(prefix + "self_attn.q_norm.weight"),
            weight(prefix + "self_attn.k_norm.weight"), config_.n_heads,
            config_.n_kv_heads, config_.head_dim, config_.rope_base);
        h = add(h, attn);

        const Tensor mlp_in = rms_norm(
            h, weight(prefix + "post_attention_layernorm.weight"), config_.rms_eps);
        const Tensor mlp = swiglu_feed_forward(
            mlp_in, weight(prefix + "mlp.gate_proj.weight"),
            weight(prefix + "mlp.up_proj.weight"), weight(prefix + "mlp.down_proj.weight"));
        h = add(h, mlp);
    }

    h = rms_norm(h, weight("model.norm.weight"), config_.rms_eps);
    const std::string output_name =
        config_.tie_word_embeddings ? "model.embed_tokens.weight" : "lm_head.weight";
    return matmul_transposed_b(h, weight(output_name));  // logits [seq, vocab]
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

std::string serialize_qwen_config(const QwenConfig& config) {
    nlohmann::json root;
    root["architectures"] = nlohmann::json::array({"Qwen3ForCausalLM"});
    root["model_type"] = "qwen3";
    root["vocab_size"] = config.vocab_size;
    root["hidden_size"] = config.d_model;
    root["num_hidden_layers"] = config.n_layers;
    root["num_attention_heads"] = config.n_heads;
    root["num_key_value_heads"] = config.n_kv_heads;
    root["head_dim"] = config.head_dim;
    root["intermediate_size"] = config.d_ff;
    root["rms_norm_eps"] = config.rms_eps;
    root["rope_theta"] = config.rope_base;
    root["tie_word_embeddings"] = config.tie_word_embeddings;
    return root.dump(2);
}

QwenInference::QwenInference(const QwenModel& model)
    : model_(model), config_(model.config()) {
    key_cache_.resize(static_cast<std::size_t>(config_.n_layers));
    value_cache_.resize(static_cast<std::size_t>(config_.n_layers));
}

void QwenInference::reset() {
    position_ = 0;
    for (auto& cache : key_cache_) cache.clear();
    for (auto& cache : value_cache_) cache.clear();
}

std::vector<float> QwenInference::step(int token_id) {
    const int d = config_.d_model;
    const int n_heads = config_.n_heads;
    const int n_kv_heads = config_.n_kv_heads;
    const int head_dim = config_.head_dim;
    const int q_dim = n_heads * head_dim;
    const int kv_dim = n_kv_heads * head_dim;
    const int group = n_heads / n_kv_heads;
    const int pos = position_;
    const float scale = 1.0F / std::sqrt(static_cast<float>(head_dim));
    const auto& weight = [&](const std::string& name) -> const std::vector<float>& {
        return model_.weight(name).node()->data;
    };

    // Embed the token (row token_id of the embedding table).
    const std::vector<float>& embed = weight("model.embed_tokens.weight");
    std::vector<float> x(embed.begin() + static_cast<std::size_t>(token_id) * d,
                         embed.begin() + static_cast<std::size_t>(token_id + 1) * d);

    for (int layer = 0; layer < config_.n_layers; ++layer) {
        const std::string prefix = "model.layers." + std::to_string(layer) + ".";

        // --- attention ---
        std::vector<float> normed = x;
        rms_norm_inplace(normed, weight(prefix + "input_layernorm.weight"),
                         config_.rms_eps);
        std::vector<float> q =
            matvec_transposed(weight(prefix + "self_attn.q_proj.weight"), normed, q_dim, d);
        std::vector<float> k =
            matvec_transposed(weight(prefix + "self_attn.k_proj.weight"), normed, kv_dim, d);
        std::vector<float> v =
            matvec_transposed(weight(prefix + "self_attn.v_proj.weight"), normed, kv_dim, d);

        // Per-head QK-norm (default eps 1e-5, matching the autograd GQA) then RoPE.
        const std::vector<float>& q_norm = weight(prefix + "self_attn.q_norm.weight");
        const std::vector<float>& k_norm = weight(prefix + "self_attn.k_norm.weight");
        for (int h = 0; h < n_heads; ++h) {
            std::vector<float> head(q.begin() + h * head_dim, q.begin() + (h + 1) * head_dim);
            rms_norm_inplace(head, q_norm, 1e-5F);
            rope_inplace(head.data(), head_dim, pos, config_.rope_base);
            std::copy(head.begin(), head.end(), q.begin() + h * head_dim);
        }
        for (int kh = 0; kh < n_kv_heads; ++kh) {
            std::vector<float> head(k.begin() + kh * head_dim, k.begin() + (kh + 1) * head_dim);
            rms_norm_inplace(head, k_norm, 1e-5F);
            rope_inplace(head.data(), head_dim, pos, config_.rope_base);
            std::copy(head.begin(), head.end(), k.begin() + kh * head_dim);
        }

        // Append this position's K/V to the layer cache.
        key_cache_[layer].insert(key_cache_[layer].end(), k.begin(), k.end());
        value_cache_[layer].insert(value_cache_[layer].end(), v.begin(), v.end());

        // Attend each query head over cached positions 0..pos of its KV head.
        std::vector<float> context(static_cast<std::size_t>(q_dim), 0.0F);
        for (int h = 0; h < n_heads; ++h) {
            const int kv_head = h / group;
            const float* q_head = q.data() + h * head_dim;
            std::vector<float> scores(static_cast<std::size_t>(pos) + 1);
            float max_score = -1e30F;
            for (int p = 0; p <= pos; ++p) {
                const float* k_p = key_cache_[layer].data() +
                                   static_cast<std::size_t>(p) * kv_dim + kv_head * head_dim;
                float s = 0.0F;
                for (int i = 0; i < head_dim; ++i) s += q_head[i] * k_p[i];
                s *= scale;
                scores[p] = s;
                if (s > max_score) max_score = s;
            }
            float sum_exp = 0.0F;
            for (float& s : scores) {
                s = std::exp(s - max_score);
                sum_exp += s;
            }
            float* context_head = context.data() + h * head_dim;
            for (int p = 0; p <= pos; ++p) {
                const float w = scores[p] / sum_exp;
                const float* v_p = value_cache_[layer].data() +
                                   static_cast<std::size_t>(p) * kv_dim + kv_head * head_dim;
                for (int i = 0; i < head_dim; ++i) context_head[i] += w * v_p[i];
            }
        }
        const std::vector<float> attn_out =
            matvec_transposed(weight(prefix + "self_attn.o_proj.weight"), context, d, q_dim);
        for (int i = 0; i < d; ++i) x[i] += attn_out[i];

        // --- SwiGLU MLP ---
        std::vector<float> normed2 = x;
        rms_norm_inplace(normed2, weight(prefix + "post_attention_layernorm.weight"),
                         config_.rms_eps);
        std::vector<float> gate =
            matvec_transposed(weight(prefix + "mlp.gate_proj.weight"), normed2, config_.d_ff, d);
        const std::vector<float> up =
            matvec_transposed(weight(prefix + "mlp.up_proj.weight"), normed2, config_.d_ff, d);
        for (int i = 0; i < config_.d_ff; ++i) {
            const float g = gate[i];
            gate[i] = (g / (1.0F + std::exp(-g))) * up[i];  // silu(g) * up
        }
        const std::vector<float> mlp =
            matvec_transposed(weight(prefix + "mlp.down_proj.weight"), gate, d, config_.d_ff);
        for (int i = 0; i < d; ++i) x[i] += mlp[i];
    }

    rms_norm_inplace(x, weight("model.norm.weight"), config_.rms_eps);
    const std::string output_name =
        config_.tie_word_embeddings ? "model.embed_tokens.weight" : "lm_head.weight";
    std::vector<float> logits =
        matvec_transposed(weight(output_name), x, config_.vocab_size, d);
    ++position_;
    return logits;
}

std::vector<int> QwenInference::generate_greedy(const std::vector<int>& prompt,
                                                int max_new_tokens, int eos_id) {
    reset();
    std::vector<float> logits;
    for (int token : prompt) logits = step(token);  // prefill

    std::vector<int> generated;
    for (int produced = 0; produced < max_new_tokens; ++produced) {
        int best = 0;
        for (int c = 1; c < config_.vocab_size; ++c) {
            if (logits[c] > logits[best]) best = c;
        }
        generated.push_back(best);
        if (eos_id >= 0 && best == eos_id) break;
        logits = step(best);
    }
    return generated;
}

std::vector<int> QwenInference::generate(const std::vector<int>& prompt,
                                         int max_new_tokens, float temperature,
                                         std::mt19937& rng, int eos_id) {
    reset();
    std::vector<float> logits;
    for (int token : prompt) logits = step(token);  // prefill
    const int vocab = config_.vocab_size;

    std::vector<int> generated;
    for (int produced = 0; produced < max_new_tokens; ++produced) {
        int next = 0;
        if (temperature <= 0.0F) {
            for (int c = 1; c < vocab; ++c) {
                if (logits[c] > logits[next]) next = c;
            }
        } else {
            float max_logit = logits[0];
            for (int c = 1; c < vocab; ++c) max_logit = std::max(max_logit, logits[c]);
            std::vector<double> probs(static_cast<std::size_t>(vocab));
            double sum_exp = 0.0;
            for (int c = 0; c < vocab; ++c) {
                probs[c] = std::exp((logits[c] - max_logit) / temperature);
                sum_exp += probs[c];
            }
            std::uniform_real_distribution<double> uniform(0.0, sum_exp);
            const double threshold = uniform(rng);
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
        if (eos_id >= 0 && next == eos_id) break;
        logits = step(next);
    }
    return generated;
}

bool save_qwen_model(const QwenModel& model, const std::string& directory, std::string* error,
                     const std::string& source_directory) {
    const auto fail = [&](std::string reason) {
        if (error != nullptr) *error = std::move(reason);
        return false;
    };

    const std::filesystem::path root(directory);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return fail("save_qwen: cannot create directory " + directory + ": " +
                    ec.message());
    }

    std::ofstream config_out(root / "config.json", std::ios::binary);
    if (!config_out) {
        return fail("save_qwen: cannot write " + (root / "config.json").string());
    }
    config_out << serialize_qwen_config(model.config());
    if (!config_out) {
        return fail("save_qwen: failed writing " + (root / "config.json").string());
    }

    if (!write_safetensors_file((root / "model.safetensors").string(),
                                model.to_safetensors())) {
        return fail("save_qwen: cannot write " +
                    (root / "model.safetensors").string());
    }

    // Carry the tokenizer over, so the saved directory is a checkpoint rather than a bag of
    // weights. A missing tokenizer is not a load error you can diagnose from the message — the
    // model appears to load and then nothing can encode a prompt for it.
    if (!source_directory.empty()) {
        const std::filesystem::path source(source_directory);
        for (const char* name : {"tokenizer.json", "tokenizer_config.json", "generation_config.json",
                                 "vocab.json", "merges.txt"}) {
            const std::filesystem::path from = source / name;
            std::error_code exists_ec;
            if (!std::filesystem::exists(from, exists_ec) || exists_ec) continue;  // optional
            std::error_code copy_ec;
            std::filesystem::copy_file(from, root / name,
                                       std::filesystem::copy_options::overwrite_existing, copy_ec);
            if (copy_ec) {
                return fail("save_qwen: cannot copy " + from.string() + ": " + copy_ec.message());
            }
        }
    }
    return true;
}

}  // namespace zetta::autograd
