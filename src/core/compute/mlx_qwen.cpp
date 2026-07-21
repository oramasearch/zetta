#include "zetta/compute/mlx_qwen.hpp"

#if defined(ZETTA_WITH_MLX)

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/compute/safetensors.hpp"

namespace zetta::compute::model {

namespace mx = mlx::core;

namespace {

// RMSNorm over the last axis, scaled by `weight` (broadcasts over leading axes).
mx::array rms_norm(const mx::array& x, const mx::array& weight, float eps) {
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

        const mx::array normed = rms_norm(x, weight(p + "input_layernorm.weight"), config_.rms_eps);
        // HF weights are [out, in]; project as x·Wᵀ.
        mx::array q = mx::matmul(normed, mx::transpose(weight(p + "self_attn.q_proj.weight")));
        mx::array k = mx::matmul(normed, mx::transpose(weight(p + "self_attn.k_proj.weight")));
        mx::array v = mx::matmul(normed, mx::transpose(weight(p + "self_attn.v_proj.weight")));
        q = mx::reshape(q, mx::Shape{len, nh, hd});
        k = mx::reshape(k, mx::Shape{len, nkv, hd});
        v = mx::reshape(v, mx::Shape{len, nkv, hd});
        // Per-head QK-norm (eps 1e-5, matching the CPU path) then RoPE.
        q = rms_norm(q, weight(p + "self_attn.q_norm.weight"), 1e-5F);
        k = rms_norm(k, weight(p + "self_attn.k_norm.weight"), 1e-5F);
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
        const mx::array normed2 = rms_norm(x, weight(p + "post_attention_layernorm.weight"), config_.rms_eps);
        const mx::array gate = mx::matmul(normed2, mx::transpose(weight(p + "mlp.gate_proj.weight")));
        const mx::array up = mx::matmul(normed2, mx::transpose(weight(p + "mlp.up_proj.weight")));
        const mx::array silu = mx::multiply(gate, mx::sigmoid(gate));
        const mx::array mlp =
            mx::matmul(mx::multiply(silu, up), mx::transpose(weight(p + "mlp.down_proj.weight")));
        x = x + mlp;
    }

    x = rms_norm(x, weight("model.norm.weight"), config_.rms_eps);
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

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
