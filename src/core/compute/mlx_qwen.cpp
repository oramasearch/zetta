#include "zetta/compute/mlx_qwen.hpp"

#if defined(ZETTA_WITH_MLX)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/safetensors.hpp"

namespace zetta::compute::model {

namespace mx = mlx::core;

namespace {

// RMSNorm over the last axis, scaled by `weight` (broadcasts over leading axes).
mx::array qwen_rms_norm(const mx::array& x, const mx::array& weight, float eps) {
    const mx::array mean_square =
        mx::mean(mx::square(x), std::vector<int>{-1}, /*keepdims=*/true);
    return mx::multiply(mx::multiply(x, mx::rsqrt(mean_square + eps)), weight);
}

// Rotate-half RoPE on x [len, heads, head_dim]; cos/sin are [len, 1, head_dim/2].
mx::array apply_rope(const mx::array& x, const mx::array& cos, const mx::array& sin,
                     int len, int heads, int head_dim) {
    const int half = head_dim / 2;
    const mx::array x1 = mx::slice(x, mx::Shape{0, 0, 0}, mx::Shape{len, heads, half});
    const mx::array x2 =
        mx::slice(x, mx::Shape{0, 0, half}, mx::Shape{len, heads, head_dim});
    const mx::array r1 = mx::multiply(x1, cos) - mx::multiply(x2, sin);
    const mx::array r2 = mx::multiply(x2, cos) + mx::multiply(x1, sin);
    return mx::concatenate(std::vector<mx::array>{r1, r2}, -1);
}

// Expand n_kv heads to n_heads (GQA): [T, n_kv, hd] -> [T, n_heads, hd], repeating
// each kv head `group` times so query head h reads kv head h/group.
mx::array expand_kv(const mx::array& x, int t, int n_kv, int group, int head_dim) {
    mx::array e = mx::reshape(x, mx::Shape{t, n_kv, 1, head_dim});
    e = mx::broadcast_to(e, mx::Shape{t, n_kv, group, head_dim});
    return mx::reshape(e, mx::Shape{t, n_kv * group, head_dim});
}

}  // namespace

MlxQwenInference::MlxQwenInference(QwenParams config,
                                  std::map<std::string, mx::array> weights,
                                  mx::array inv_freq)
    : config_(config),
      weights_(std::move(weights)),
      key_cache_(static_cast<std::size_t>(config.n_layers),
                 mx::zeros(mx::Shape{1}, mx::float32)),
      value_cache_(static_cast<std::size_t>(config.n_layers),
                   mx::zeros(mx::Shape{1}, mx::float32)),
      inv_freq_(std::move(inv_freq)) {}

std::optional<MlxQwenInference> MlxQwenInference::from_directory(
    const std::string& directory, std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<MlxQwenInference> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };

    std::ifstream config_in(directory + "/config.json", std::ios::binary);
    if (!config_in) return fail("mlx_qwen: cannot open " + directory + "/config.json");
    std::ostringstream config_buffer;
    config_buffer << config_in.rdbuf();
    const nlohmann::json cfg =
        nlohmann::json::parse(config_buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (cfg.is_discarded() || !cfg.is_object()) {
        return fail("mlx_qwen: invalid config.json");
    }

    QwenParams config;
    config.vocab_size = cfg.value("vocab_size", 0);
    config.d_model = cfg.value("hidden_size", 0);
    config.n_layers = cfg.value("num_hidden_layers", 0);
    config.n_heads = cfg.value("num_attention_heads", 0);
    config.n_kv_heads = cfg.value("num_key_value_heads", 0);
    config.head_dim = cfg.contains("head_dim") && cfg["head_dim"].is_number_integer()
                          ? cfg["head_dim"].get<int>()
                          : (config.n_heads > 0 ? config.d_model / config.n_heads : 0);
    config.d_ff = cfg.value("intermediate_size", 0);
    config.rms_eps = cfg.value("rms_norm_eps", 1e-6F);
    config.rope_base = cfg.value("rope_theta", 1000000.0F);
    config.tie_word_embeddings = cfg.value("tie_word_embeddings", false);
    if (config.n_heads <= 0 || config.n_kv_heads <= 0 || config.head_dim <= 0 ||
        config.n_heads % config.n_kv_heads != 0) {
        return fail("mlx_qwen: invalid head configuration in config.json");
    }

    std::map<std::string, mx::array> weights =
        read_safetensors_file(directory + "/model.safetensors");
    if (weights.empty()) {
        return fail("mlx_qwen: no tensors in " + directory + "/model.safetensors");
    }

    // RoPE inverse frequencies: base^(-2i/head_dim), i in [0, head_dim/2).
    const int half = config.head_dim / 2;
    std::vector<float> freqs(static_cast<std::size_t>(half));
    for (int i = 0; i < half; ++i) {
        freqs[i] = static_cast<float>(
            std::pow(static_cast<double>(config.rope_base), -2.0 * i / config.head_dim));
    }
    mx::array inv_freq(freqs.data(), mx::Shape{half}, mx::float32);

    return MlxQwenInference(config, std::move(weights), std::move(inv_freq));
}

void MlxQwenInference::reset() {
    position_ = 0;
    for (auto& cache : key_cache_) cache = mx::zeros(mx::Shape{1}, mx::float32);
    for (auto& cache : value_cache_) cache = mx::zeros(mx::Shape{1}, mx::float32);
}

std::vector<float> MlxQwenInference::forward(const std::vector<int>& tokens) {
    const int len = static_cast<int>(tokens.size());
    const int d = config_.d_model;
    const int nh = config_.n_heads;
    const int nkv = config_.n_kv_heads;
    const int hd = config_.head_dim;
    const int q_dim = nh * hd;
    const int group = nh / nkv;
    const int base = position_;
    const int total = base + len;  // cache length after this forward
    const float scale = 1.0F / std::sqrt(static_cast<float>(hd));
    const auto& weight = [&](const std::string& n) -> const mx::array& {
        return weights_.at(n);
    };

    // Embed tokens -> [len, d].
    std::vector<int> ids(tokens.begin(), tokens.end());
    const mx::array token_arr(ids.data(), mx::Shape{len}, mx::int32);
    mx::array x = mx::take(weight("model.embed_tokens.weight"), token_arr, 0);

    // RoPE cos/sin for the new positions -> [len, 1, head_dim/2] (broadcast over heads).
    std::vector<float> pos(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) pos[i] = static_cast<float>(base + i);
    const mx::array positions(pos.data(), mx::Shape{len, 1}, mx::float32);
    const mx::array angles =
        mx::multiply(positions, mx::reshape(inv_freq_, mx::Shape{1, hd / 2}));
    const mx::array cos = mx::reshape(mx::cos(angles), mx::Shape{len, 1, hd / 2});
    const mx::array sin = mx::reshape(mx::sin(angles), mx::Shape{len, 1, hd / 2});

    // Causal mask [1, len, total]: new token i (abs base+i) attends to cache 0..base+i.
    std::vector<float> mask_data(static_cast<std::size_t>(len) * total, 0.0F);
    for (int i = 0; i < len; ++i) {
        for (int j = 0; j < total; ++j) {
            if (j > base + i) mask_data[i * total + j] = -1e9F;
        }
    }
    const mx::array mask(mask_data.data(), mx::Shape{1, len, total}, mx::float32);

    for (int layer = 0; layer < config_.n_layers; ++layer) {
        const std::string p = "model.layers." + std::to_string(layer) + ".";

        const mx::array normed = qwen_rms_norm(x, weight(p + "input_layernorm.weight"), config_.rms_eps);
        // HF weights are [out, in]; project as x·Wᵀ.
        mx::array q = mx::matmul(normed, mx::transpose(weight(p + "self_attn.q_proj.weight")));
        mx::array k = mx::matmul(normed, mx::transpose(weight(p + "self_attn.k_proj.weight")));
        mx::array v = mx::matmul(normed, mx::transpose(weight(p + "self_attn.v_proj.weight")));
        q = mx::reshape(q, mx::Shape{len, nh, hd});
        k = mx::reshape(k, mx::Shape{len, nkv, hd});
        v = mx::reshape(v, mx::Shape{len, nkv, hd});
        // Per-head QK-norm (eps 1e-5, matching the CPU path) then RoPE.
        q = qwen_rms_norm(q, weight(p + "self_attn.q_norm.weight"), 1e-5F);
        k = qwen_rms_norm(k, weight(p + "self_attn.k_norm.weight"), 1e-5F);
        q = apply_rope(q, cos, sin, len, nh, hd);
        k = apply_rope(k, cos, sin, len, nkv, hd);

        // Append to the KV cache (which lives in MLX arrays between steps).
        if (base == 0) {
            key_cache_[layer] = k;
            value_cache_[layer] = v;
        } else {
            key_cache_[layer] = mx::concatenate(std::vector<mx::array>{key_cache_[layer], k}, 0);
            value_cache_[layer] = mx::concatenate(std::vector<mx::array>{value_cache_[layer], v}, 0);
        }

        // Attention over the full cache. Expand kv heads to nh, put heads first.
        const mx::array ck = expand_kv(key_cache_[layer], total, nkv, group, hd);   // [T,nh,hd]
        const mx::array cv = expand_kv(value_cache_[layer], total, nkv, group, hd);
        const mx::array qh = mx::transpose(q, std::vector<int>{1, 0, 2});   // [nh,len,hd]
        const mx::array kh = mx::transpose(ck, std::vector<int>{1, 0, 2});  // [nh,T,hd]
        const mx::array vh = mx::transpose(cv, std::vector<int>{1, 0, 2});  // [nh,T,hd]
        mx::array scores =
            mx::matmul(qh, mx::transpose(kh, std::vector<int>{0, 2, 1})) * scale;  // [nh,len,T]
        scores = scores + mask;
        const mx::array attn = mx::softmax(scores, std::vector<int>{-1});  // [nh,len,T]
        mx::array context = mx::matmul(attn, vh);                          // [nh,len,hd]
        context = mx::transpose(context, std::vector<int>{1, 0, 2});       // [len,nh,hd]
        context = mx::reshape(context, mx::Shape{len, q_dim});
        const mx::array attn_out =
            mx::matmul(context, mx::transpose(weight(p + "self_attn.o_proj.weight")));
        x = x + attn_out;

        // SwiGLU MLP.
        const mx::array normed2 = qwen_rms_norm(x, weight(p + "post_attention_layernorm.weight"), config_.rms_eps);
        const mx::array gate = mx::matmul(normed2, mx::transpose(weight(p + "mlp.gate_proj.weight")));
        const mx::array up = mx::matmul(normed2, mx::transpose(weight(p + "mlp.up_proj.weight")));
        const mx::array silu = mx::multiply(gate, mx::sigmoid(gate));
        const mx::array mlp =
            mx::matmul(mx::multiply(silu, up), mx::transpose(weight(p + "mlp.down_proj.weight")));
        x = x + mlp;
    }

    x = qwen_rms_norm(x, weight("model.norm.weight"), config_.rms_eps);
    const std::string out_name =
        config_.tie_word_embeddings ? "model.embed_tokens.weight" : "lm_head.weight";
    const mx::array logits = mx::matmul(x, mx::transpose(weight(out_name)));  // [len, vocab]
    mx::array last = mx::reshape(
        mx::slice(logits, mx::Shape{len - 1, 0}, mx::Shape{len, config_.vocab_size}),
        mx::Shape{config_.vocab_size});

    // One eval: materialize the next-token logits AND the updated caches (so the
    // next step builds on evaluated arrays rather than an ever-growing graph).
    std::vector<mx::array> to_eval = key_cache_;
    to_eval.insert(to_eval.end(), value_cache_.begin(), value_cache_.end());
    to_eval.push_back(last);
    mx::eval(to_eval);

    position_ += len;
    return std::vector<float>(last.data<float>(), last.data<float>() + config_.vocab_size);
}

std::vector<int> MlxQwenInference::generate_greedy(const std::vector<int>& prompt,
                                                  int max_new_tokens, int eos_id) {
    reset();
    std::vector<float> logits = forward(prompt);  // prefill
    std::vector<int> generated;
    for (int produced = 0; produced < max_new_tokens; ++produced) {
        int best = 0;
        for (int c = 1; c < config_.vocab_size; ++c) {
            if (logits[c] > logits[best]) best = c;
        }
        generated.push_back(best);
        if (eos_id >= 0 && best == eos_id) break;
        logits = forward(std::vector<int>{best});
    }
    return generated;
}

// --- Fused MLX Qwen training -------------------------------------------------

MlxQwenModel::MlxQwenModel(QwenParams config, std::vector<mx::array> params,
                           mx::array inv_freq)
    : config_(config),
      params_(std::move(params)),
      inv_freq_(std::move(inv_freq)),
      key_cache_(static_cast<std::size_t>(config.n_layers),
                 mx::zeros(mx::Shape{1}, mx::float32)),
      value_cache_(static_cast<std::size_t>(config.n_layers),
                   mx::zeros(mx::Shape{1}, mx::float32)) {}

std::optional<MlxQwenModel> MlxQwenModel::from_directory(const std::string& directory,
                                                        std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<MlxQwenModel> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    std::ifstream config_in(directory + "/config.json", std::ios::binary);
    if (!config_in) return fail("mlx_qwen: cannot open " + directory + "/config.json");
    std::ostringstream config_buffer;
    config_buffer << config_in.rdbuf();
    const nlohmann::json cfg =
        nlohmann::json::parse(config_buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (cfg.is_discarded() || !cfg.is_object()) return fail("mlx_qwen: invalid config.json");

    QwenParams config;
    config.vocab_size = cfg.value("vocab_size", 0);
    config.d_model = cfg.value("hidden_size", 0);
    config.n_layers = cfg.value("num_hidden_layers", 0);
    config.n_heads = cfg.value("num_attention_heads", 0);
    config.n_kv_heads = cfg.value("num_key_value_heads", 0);
    config.head_dim = cfg.contains("head_dim") && cfg["head_dim"].is_number_integer()
                          ? cfg["head_dim"].get<int>()
                          : (config.n_heads > 0 ? config.d_model / config.n_heads : 0);
    config.d_ff = cfg.value("intermediate_size", 0);
    config.rms_eps = cfg.value("rms_norm_eps", 1e-6F);
    config.rope_base = cfg.value("rope_theta", 1000000.0F);
    config.tie_word_embeddings = cfg.value("tie_word_embeddings", false);
    if (config.n_heads <= 0 || config.n_kv_heads <= 0 || config.head_dim <= 0 ||
        config.n_heads % config.n_kv_heads != 0) {
        return fail("mlx_qwen: invalid head configuration");
    }

    std::map<std::string, mx::array> w =
        read_safetensors_file(directory + "/model.safetensors");
    if (w.empty()) return fail("mlx_qwen: no tensors in model.safetensors");

    // Order the parameters: [embed, per-layer x11, final norm, (lm_head)].
    std::vector<mx::array> params;
    const auto push = [&](const std::string& name) -> bool {
        const auto found = w.find(name);
        if (found == w.end()) return false;
        params.push_back(found->second);
        return true;
    };
    if (!push("model.embed_tokens.weight")) return fail("mlx_qwen: missing embed_tokens");
    for (int layer = 0; layer < config.n_layers; ++layer) {
        const std::string p = "model.layers." + std::to_string(layer) + ".";
        const char* suffixes[] = {
            "input_layernorm.weight", "self_attn.q_proj.weight", "self_attn.k_proj.weight",
            "self_attn.v_proj.weight", "self_attn.o_proj.weight", "self_attn.q_norm.weight",
            "self_attn.k_norm.weight", "post_attention_layernorm.weight",
            "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight"};
        for (const char* s : suffixes) {
            if (!push(p + s)) return fail("mlx_qwen: missing " + p + s);
        }
    }
    if (!push("model.norm.weight")) return fail("mlx_qwen: missing model.norm");
    if (!config.tie_word_embeddings && !push("lm_head.weight")) {
        return fail("mlx_qwen: missing lm_head");
    }

    const int half = config.head_dim / 2;
    std::vector<float> freqs(static_cast<std::size_t>(half));
    for (int i = 0; i < half; ++i) {
        freqs[i] = static_cast<float>(
            std::pow(static_cast<double>(config.rope_base), -2.0 * i / config.head_dim));
    }
    mx::array inv_freq(freqs.data(), mx::Shape{half}, mx::float32);
    return MlxQwenModel(config, std::move(params), std::move(inv_freq));
}

mx::array MlxQwenModel::forward(const std::vector<mx::array>& params,
                                const mx::array& token_ids) const {
    const int len = token_ids.shape(0);
    const int d = config_.d_model, nh = config_.n_heads, nkv = config_.n_kv_heads;
    const int hd = config_.head_dim, q_dim = nh * hd, group = nh / nkv;
    const float scale = 1.0F / std::sqrt(static_cast<float>(hd));

    mx::array x = mx::take(params[0], token_ids, 0);  // [len, d]

    std::vector<float> pos(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) pos[i] = static_cast<float>(i);
    const mx::array positions(pos.data(), mx::Shape{len, 1}, mx::float32);
    const mx::array angles =
        mx::multiply(positions, mx::reshape(inv_freq_, mx::Shape{1, hd / 2}));
    const mx::array cos = mx::reshape(mx::cos(angles), mx::Shape{len, 1, hd / 2});
    const mx::array sin = mx::reshape(mx::sin(angles), mx::Shape{len, 1, hd / 2});

    std::vector<float> mask_data(static_cast<std::size_t>(len) * len, 0.0F);
    for (int i = 0; i < len; ++i) {
        for (int j = i + 1; j < len; ++j) mask_data[i * len + j] = -1e9F;
    }
    const mx::array mask(mask_data.data(), mx::Shape{1, len, len}, mx::float32);

    for (int layer = 0; layer < config_.n_layers; ++layer) {
        const int b = 1 + 11 * layer;
        const mx::array normed = qwen_rms_norm(x, params[b + 0], config_.rms_eps);
        mx::array q = mx::matmul(normed, mx::transpose(params[b + 1]));
        mx::array k = mx::matmul(normed, mx::transpose(params[b + 2]));
        mx::array v = mx::matmul(normed, mx::transpose(params[b + 3]));
        q = qwen_rms_norm(mx::reshape(q, mx::Shape{len, nh, hd}), params[b + 5], 1e-5F);
        k = qwen_rms_norm(mx::reshape(k, mx::Shape{len, nkv, hd}), params[b + 6], 1e-5F);
        v = mx::reshape(v, mx::Shape{len, nkv, hd});
        q = apply_rope(q, cos, sin, len, nh, hd);
        k = apply_rope(k, cos, sin, len, nkv, hd);
        const mx::array ck = expand_kv(k, len, nkv, group, hd);
        const mx::array cv = expand_kv(v, len, nkv, group, hd);
        const mx::array qh = mx::transpose(q, std::vector<int>{1, 0, 2});
        const mx::array kh = mx::transpose(ck, std::vector<int>{1, 0, 2});
        const mx::array vh = mx::transpose(cv, std::vector<int>{1, 0, 2});
        mx::array scores =
            mx::matmul(qh, mx::transpose(kh, std::vector<int>{0, 2, 1})) * scale + mask;
        const mx::array attn = mx::softmax(scores, std::vector<int>{-1});
        mx::array context = mx::transpose(mx::matmul(attn, vh), std::vector<int>{1, 0, 2});
        context = mx::reshape(context, mx::Shape{len, q_dim});
        x = x + mx::matmul(context, mx::transpose(params[b + 4]));
        const mx::array normed2 = qwen_rms_norm(x, params[b + 7], config_.rms_eps);
        const mx::array gate = mx::matmul(normed2, mx::transpose(params[b + 8]));
        const mx::array up = mx::matmul(normed2, mx::transpose(params[b + 9]));
        const mx::array silu = mx::multiply(gate, mx::sigmoid(gate));
        x = x + mx::matmul(mx::multiply(silu, up), mx::transpose(params[b + 10]));
    }

    const int norm_idx = 1 + 11 * config_.n_layers;
    x = qwen_rms_norm(x, params[norm_idx], config_.rms_eps);
    const mx::array& out_w = config_.tie_word_embeddings ? params[0] : params[norm_idx + 1];
    return mx::matmul(x, mx::transpose(out_w));  // [len, vocab]
}

mx::array MlxQwenModel::next_token_loss(const std::vector<mx::array>& params,
                                        const mx::array& token_ids) const {
    const int len = token_ids.shape(0);
    const mx::array logits = forward(params, token_ids);
    const mx::array predicted =
        mx::slice(logits, mx::Shape{0, 0}, mx::Shape{len - 1, config_.vocab_size});
    const mx::array targets = mx::slice(token_ids, mx::Shape{1}, mx::Shape{len});
    const mx::array log_probs =
        predicted - mx::logsumexp(predicted, std::vector<int>{-1}, true);
    const mx::array chosen =
        mx::take_along_axis(log_probs, mx::reshape(targets, mx::Shape{len - 1, 1}), 1);
    return -mx::mean(chosen, /*keepdims=*/false);
}

// --- Fused GPU RLVR: KV-cached rollouts + one value_and_grad over the group ------

std::vector<float> MlxQwenModel::cached_step(int token_id) {
    const int d = config_.d_model, nh = config_.n_heads, nkv = config_.n_kv_heads;
    const int hd = config_.head_dim, q_dim = nh * hd, group = nh / nkv;
    const int pos = cache_pos_, total = pos + 1;
    const float scale = 1.0F / std::sqrt(static_cast<float>(hd));
    (void)d;

    const int id[1] = {token_id};
    const mx::array token_arr(id, mx::Shape{1}, mx::int32);
    mx::array x = mx::take(params_[0], token_arr, 0);  // [1, d]

    const float pf[1] = {static_cast<float>(pos)};
    const mx::array positions(pf, mx::Shape{1, 1}, mx::float32);
    const mx::array angles =
        mx::multiply(positions, mx::reshape(inv_freq_, mx::Shape{1, hd / 2}));
    const mx::array cos = mx::reshape(mx::cos(angles), mx::Shape{1, 1, hd / 2});
    const mx::array sin = mx::reshape(mx::sin(angles), mx::Shape{1, 1, hd / 2});

    for (int layer = 0; layer < config_.n_layers; ++layer) {
        const int b = 1 + 11 * layer;
        const mx::array normed = qwen_rms_norm(x, params_[b + 0], config_.rms_eps);
        mx::array q = mx::matmul(normed, mx::transpose(params_[b + 1]));
        mx::array k = mx::matmul(normed, mx::transpose(params_[b + 2]));
        mx::array v = mx::matmul(normed, mx::transpose(params_[b + 3]));
        q = qwen_rms_norm(mx::reshape(q, mx::Shape{1, nh, hd}), params_[b + 5], 1e-5F);
        k = qwen_rms_norm(mx::reshape(k, mx::Shape{1, nkv, hd}), params_[b + 6], 1e-5F);
        v = mx::reshape(v, mx::Shape{1, nkv, hd});
        q = apply_rope(q, cos, sin, 1, nh, hd);
        k = apply_rope(k, cos, sin, 1, nkv, hd);
        if (pos == 0) {
            key_cache_[layer] = k;
            value_cache_[layer] = v;
        } else {
            key_cache_[layer] = mx::concatenate(std::vector<mx::array>{key_cache_[layer], k}, 0);
            value_cache_[layer] = mx::concatenate(std::vector<mx::array>{value_cache_[layer], v}, 0);
        }
        const mx::array ck = expand_kv(key_cache_[layer], total, nkv, group, hd);
        const mx::array cv = expand_kv(value_cache_[layer], total, nkv, group, hd);
        const mx::array qh = mx::transpose(q, std::vector<int>{1, 0, 2});
        const mx::array kh = mx::transpose(ck, std::vector<int>{1, 0, 2});
        const mx::array vh = mx::transpose(cv, std::vector<int>{1, 0, 2});
        const mx::array scores =
            mx::matmul(qh, mx::transpose(kh, std::vector<int>{0, 2, 1})) * scale;  // [nh,1,T]
        const mx::array attn = mx::softmax(scores, std::vector<int>{-1});
        mx::array context = mx::transpose(mx::matmul(attn, vh), std::vector<int>{1, 0, 2});
        context = mx::reshape(context, mx::Shape{1, q_dim});
        x = x + mx::matmul(context, mx::transpose(params_[b + 4]));
        const mx::array normed2 = qwen_rms_norm(x, params_[b + 7], config_.rms_eps);
        const mx::array gate = mx::matmul(normed2, mx::transpose(params_[b + 8]));
        const mx::array up = mx::matmul(normed2, mx::transpose(params_[b + 9]));
        const mx::array silu = mx::multiply(gate, mx::sigmoid(gate));
        x = x + mx::matmul(mx::multiply(silu, up), mx::transpose(params_[b + 10]));
    }
    const int norm_idx = 1 + 11 * config_.n_layers;
    x = qwen_rms_norm(x, params_[norm_idx], config_.rms_eps);
    const mx::array& out_w = config_.tie_word_embeddings ? params_[0] : params_[norm_idx + 1];
    mx::array last = mx::reshape(mx::matmul(x, mx::transpose(out_w)), mx::Shape{config_.vocab_size});

    std::vector<mx::array> to_eval = key_cache_;
    to_eval.insert(to_eval.end(), value_cache_.begin(), value_cache_.end());
    to_eval.push_back(last);
    mx::eval(to_eval);
    ++cache_pos_;
    return std::vector<float>(last.data<float>(), last.data<float>() + config_.vocab_size);
}

std::vector<int> MlxQwenModel::generate(const std::vector<int>& prompt, int max_new_tokens,
                                        float temperature, std::mt19937& rng, int eos_id) {
    cache_pos_ = 0;
    for (auto& c : key_cache_) c = mx::zeros(mx::Shape{1}, mx::float32);
    for (auto& c : value_cache_) c = mx::zeros(mx::Shape{1}, mx::float32);
    const int vocab = config_.vocab_size;

    std::vector<float> logits;
    for (int t : prompt) logits = cached_step(t);
    std::vector<int> generated;
    for (int produced = 0; produced < max_new_tokens; ++produced) {
        int next = 0;
        if (temperature <= 0.0F) {
            for (int c = 1; c < vocab; ++c) if (logits[c] > logits[next]) next = c;
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
                if (threshold < cumulative) { next = c; break; }
            }
        }
        generated.push_back(next);
        if (eos_id >= 0 && next == eos_id) break;
        logits = cached_step(next);
    }
    return generated;
}

mx::array MlxQwenModel::sequence_pg_loss(const std::vector<mx::array>& params,
                                         const std::vector<int>& tokens, int prompt_len,
                                         const std::vector<int>& actions,
                                         double advantage) const {
    const int t = static_cast<int>(actions.size());
    const std::vector<int> tokens32(tokens.begin(), tokens.end());
    const mx::array token_arr(tokens32.data(), mx::Shape{static_cast<int>(tokens.size())}, mx::int32);
    const mx::array logits = forward(params, token_arr);  // [len, vocab]
    // Rows that produced the completion tokens: [prompt_len-1 .. prompt_len-1+t).
    const mx::array aligned = mx::slice(logits, mx::Shape{prompt_len - 1, 0},
                                        mx::Shape{prompt_len - 1 + t, config_.vocab_size});
    const mx::array log_probs =
        aligned - mx::logsumexp(aligned, std::vector<int>{-1}, true);
    const std::vector<int> actions32(actions.begin(), actions.end());
    const mx::array action_arr(actions32.data(), mx::Shape{t, 1}, mx::int32);
    const mx::array chosen = mx::take_along_axis(log_probs, action_arr, 1);  // [t,1]
    return mx::sum(chosen, /*keepdims=*/false) * static_cast<float>(-advantage);
}

double MlxQwenModel::grpo_step(AdamW& optimizer, const std::vector<int>& prompt,
                               const CompletionReward& reward, int group_size,
                               int max_new_tokens, float temperature, std::mt19937& rng) {
    const int prompt_len = static_cast<int>(prompt.size());
    // Roll out the group with the KV cache (fast, on-GPU, detached from the graph).
    std::vector<std::vector<int>> completions;
    std::vector<double> rewards;
    double reward_sum = 0.0;
    for (int i = 0; i < group_size; ++i) {
        std::vector<int> completion = generate(prompt, max_new_tokens, temperature, rng);
        const double value = reward(prompt, completion);
        reward_sum += value;
        completions.push_back(std::move(completion));
        rewards.push_back(value);
    }
    // Group-relative advantages: (r - mean) / (std + eps).
    double mean = 0.0;
    for (double r : rewards) mean += r;
    mean /= group_size;
    double variance = 0.0;
    for (double r : rewards) variance += (r - mean) * (r - mean);
    const double stddev = std::sqrt(variance / group_size);
    std::vector<double> advantages(static_cast<std::size_t>(group_size));
    for (int i = 0; i < group_size; ++i) advantages[i] = (rewards[i] - mean) / (stddev + 1e-8);

    // One value_and_grad over the whole group (sequence policy gradient) -> AdamW.
    const auto loss_fn = [&](const std::vector<mx::array>& params) {
        mx::array total = mx::zeros(mx::Shape{}, mx::float32);
        int contributing = 0;
        for (int i = 0; i < group_size; ++i) {
            if (completions[i].empty()) continue;
            std::vector<int> full = prompt;
            full.insert(full.end(), completions[i].begin(), completions[i].end());
            total = total + sequence_pg_loss(params, full, prompt_len, completions[i],
                                             advantages[i]);
            ++contributing;
        }
        return contributing > 0 ? total * (1.0F / contributing) : total;
    };
    std::vector<int> argnums(params_.size());
    std::iota(argnums.begin(), argnums.end(), 0);
    auto [loss, gradients] = mx::value_and_grad(loss_fn, argnums)(params_);
    optimizer.update(params_, gradients);
    mx::eval(params_);

    return group_size > 0 ? reward_sum / group_size : 0.0;
}

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
