#include "zetta/compute/cuda_qwen_model.hpp"

#include <span>
#include <utility>

#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/compute/cuda_device.hpp"
#include "zetta/compute/cuda_transformer.hpp"

namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

namespace {

// A scratch buffer of `elements` floats. Allocation goes through the device, so it is served by the
// pool (#928) and freed back into it — a layer allocates the same shapes every step, so after the
// first step these cost no cudaMalloc at all.
DeviceBuffer scratch(ComputeDevice& device, std::size_t elements) {
    return device.allocate(DataType::Float32, elements);
}

// out[rows, out_features] = in[rows, in_features] * W^T, with W in Hugging Face [out, in] layout.
// Transposing W is what a naive port would do; passing transpose_b instead means the weight is
// never rewritten, which is the whole reason it can stay resident.
ComputeStatus project(ComputeDevice& device, const DeviceBuffer& in, const DeviceBuffer& weight,
                      DeviceBuffer& out, int rows, int in_features, int out_features) {
    GemmParams params;
    params.rows = static_cast<std::size_t>(rows);
    params.inner = static_cast<std::size_t>(in_features);
    params.cols = static_cast<std::size_t>(out_features);
    params.alpha = 1.0;
    params.beta = 0.0;
    params.transpose_a = false;
    params.transpose_b = true;
    return device.gemm(params, in, weight, out);
}

}  // namespace


// grad_W = grad_Y^T * X, for a projection Y = X * W^T with W in Hugging Face [out, in] layout.
// Accumulates (beta = 1), because a weight used twice in a step must sum both contributions.
ComputeStatus accumulate_weight_grad(ComputeDevice& device, const DeviceBuffer& grad_y,
                                     const DeviceBuffer& x, DeviceBuffer& grad_w, int rows,
                                     int out_features, int in_features) {
    GemmParams params;
    params.rows = static_cast<std::size_t>(out_features);
    params.inner = static_cast<std::size_t>(rows);
    params.cols = static_cast<std::size_t>(in_features);
    params.alpha = 1.0;
    params.beta = 1.0;  // accumulate
    params.transpose_a = true;
    params.transpose_b = false;
    return device.gemm(params, grad_y, x, grad_w);
}

// grad_X = grad_Y * W, the input-side gradient of the same projection.
ComputeStatus input_grad(ComputeDevice& device, const DeviceBuffer& grad_y,
                         const DeviceBuffer& weight, DeviceBuffer& grad_x, int rows,
                         int out_features, int in_features) {
    GemmParams params;
    params.rows = static_cast<std::size_t>(rows);
    params.inner = static_cast<std::size_t>(out_features);
    params.cols = static_cast<std::size_t>(in_features);
    params.alpha = 1.0;
    params.beta = 0.0;
    params.transpose_a = false;
    params.transpose_b = false;
    return device.gemm(params, grad_y, weight, grad_x);
}

CudaQwenModel CudaQwenModel::from_host(ComputeDevice& device,
                                       const zetta::autograd::QwenModel& host) {
    CudaQwenModel model;
    model.parameters_.reserve(host.parameters().size());
    for (const zetta::autograd::Tensor& parameter : host.parameters()) {
        ResidentTensor resident =
            ResidentTensor::upload(device, parameter.data(), parameter.shape());
        if (!resident.valid()) {
            // All or nothing: a partially-resident model would run and produce plausible wrong
            // numbers, which is far worse to diagnose than a refusal at load.
            return CudaQwenModel{};
        }
        model.parameters_.push_back(std::move(resident));
    }
    model.config_ = host.config();
    // Fail loudly if the registration order this class indexes arithmetically ever changes.
    const std::size_t expected =
        1 + static_cast<std::size_t>(model.config_.n_layers) * kPerLayer + 1 +
        (model.config_.tie_word_embeddings ? 0u : 1u);
    if (model.parameters_.size() != expected) return CudaQwenModel{};
    return model;
}


ComputeStatus CudaQwenModel::forward(ComputeDevice& device, const std::vector<int>& ids,
                                     ResidentTensor& logits) const {
    if (!valid()) return ComputeStatus::invalid_argument("forward: model is not resident");
    if (ids.empty()) return ComputeStatus::invalid_argument("forward: no tokens");
    const int seq = static_cast<int>(ids.size());
    const int d = config_.d_model;
    const std::size_t hidden_elements =
        static_cast<std::size_t>(seq) * static_cast<std::size_t>(d);

    // The token ids are the only host data the forward needs — a few hundred ints, against the
    // megabytes of embedded rows a host-side lookup would have had to upload.
    DeviceBuffer device_ids = device.upload<int>(std::span<const int>(ids));
    if (device_ids.empty()) return ComputeStatus::out_of_memory("forward: ids");

    DeviceBuffer embedded = device.allocate(DataType::Float32, hidden_elements);
    if (embedded.empty()) return ComputeStatus::out_of_memory("forward: embedding");
    if (const auto s = cuda_embedding(parameters_[0].buffer(), device_ids, embedded, seq, d,
                                      config_.vocab_size);
        !s.ok()) {
        return s;
    }

    // Each layer's output feeds the next directly — the hidden state never leaves the device, which
    // is the entire point of the rung-3 work.
    ResidentTensor hidden = ResidentTensor::adopt(std::move(embedded), {seq, d});
    for (int layer = 0; layer < config_.n_layers; ++layer) {
        ResidentTensor next;
        if (const auto s = forward_layer(device, hidden, layer, seq, next); !s.ok()) return s;
        hidden = std::move(next);
    }

    const std::size_t final_norm_index =
        1 + static_cast<std::size_t>(config_.n_layers) * kPerLayer;
    DeviceBuffer normed = device.allocate(DataType::Float32, hidden_elements);
    if (normed.empty()) return ComputeStatus::out_of_memory("forward: final norm");
    if (const auto s = cuda_rms_norm(hidden.buffer(), parameters_[final_norm_index].buffer(),
                                     normed, seq, d, config_.rms_eps);
        !s.ok()) {
        return s;
    }

    // Tied embeddings reuse the embedding table as the output projection, which is why the
    // parameter list has no lm_head entry in that case.
    const ResidentTensor& output_weight = config_.tie_word_embeddings
                                              ? parameters_[0]
                                              : parameters_[final_norm_index + 1];
    DeviceBuffer out = device.allocate(
        DataType::Float32,
        static_cast<std::size_t>(seq) * static_cast<std::size_t>(config_.vocab_size));
    if (out.empty()) return ComputeStatus::out_of_memory("forward: logits");
    if (const auto s = project(device, normed, output_weight.buffer(), out, seq, d,
                               config_.vocab_size);
        !s.ok()) {
        return s;
    }
    logits = ResidentTensor::adopt(std::move(out), {seq, config_.vocab_size});
    return ComputeStatus::success();
}


ComputeStatus CudaQwenModel::forward_layer_cached(ComputeDevice& device, ResidentTensor hidden,
                                                  int layer, int seq, LayerCache& cache,
                                                  ResidentTensor& out) const {
    if (!valid()) return ComputeStatus::invalid_argument("forward_layer_cached: not resident");
    if (layer < 0 || layer >= config_.n_layers) {
        return ComputeStatus::invalid_argument("forward_layer_cached: layer out of range");
    }
    const int d = config_.d_model;
    const int head_dim = config_.head_dim;
    const int q_dim = config_.n_heads * head_dim;
    const int kv_dim = config_.n_kv_heads * head_dim;
    const std::size_t tokens = static_cast<std::size_t>(seq);
    const std::size_t hidden_elements = tokens * static_cast<std::size_t>(d);

    const std::size_t base = layer_base(layer);
    const auto& input_norm = parameters_[base + 0];
    const auto& w_q = parameters_[base + 1];
    const auto& w_k = parameters_[base + 2];
    const auto& w_v = parameters_[base + 3];
    const auto& w_o = parameters_[base + 4];
    const auto& q_norm = parameters_[base + 5];
    const auto& k_norm = parameters_[base + 6];
    const auto& post_norm = parameters_[base + 7];
    const auto& w_gate = parameters_[base + 8];
    const auto& w_up = parameters_[base + 9];
    const auto& w_down = parameters_[base + 10];

    const auto need = [&](DeviceBuffer& buffer, std::size_t elements, const char* what) {
        buffer = device.allocate(DataType::Float32, elements);
        return buffer.empty() ? ComputeStatus::out_of_memory(std::string("layer: ") + what)
                              : ComputeStatus::success();
    };

    // --- attention ---------------------------------------------------------------------
    DeviceBuffer normed;
    if (const auto s = need(normed, hidden_elements, "normed"); !s.ok()) return s;
    if (const auto s = cuda_rms_norm(hidden.buffer(), input_norm.buffer(), normed, seq, d,
                                     config_.rms_eps);
        !s.ok()) {
        return s;
    }

    DeviceBuffer q, k, v, q_rope, k_rope;
    if (const auto s = need(q, tokens * static_cast<std::size_t>(q_dim), "q"); !s.ok()) return s;
    if (const auto s = need(k, tokens * static_cast<std::size_t>(kv_dim), "k"); !s.ok()) return s;
    if (const auto s = need(v, tokens * static_cast<std::size_t>(kv_dim), "v"); !s.ok()) return s;
    if (const auto s = need(q_rope, tokens * static_cast<std::size_t>(q_dim), "q_rope");
        !s.ok()) {
        return s;
    }
    if (const auto s = need(k_rope, tokens * static_cast<std::size_t>(kv_dim), "k_rope");
        !s.ok()) {
        return s;
    }
    if (const auto s = project(device, normed, w_q.buffer(), q, seq, d, q_dim); !s.ok()) return s;
    if (const auto s = project(device, normed, w_k.buffer(), k, seq, d, kv_dim); !s.ok()) return s;
    if (const auto s = project(device, normed, w_v.buffer(), v, seq, d, kv_dim); !s.ok()) return s;
    // QK-norm into a separate buffer, then RoPE into another: the backward needs to run each
    // inverse in turn, so overwriting in place would destroy what it has to undo.
    if (const auto s = cuda_rms_norm(q, q_norm.buffer(), q_rope, seq * config_.n_heads, head_dim,
                                     config_.rms_eps);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rms_norm(k, k_norm.buffer(), k_rope, seq * config_.n_kv_heads, head_dim,
                                     config_.rms_eps);
        !s.ok()) {
        return s;
    }
    DeviceBuffer q_final, k_final;
    if (const auto s = need(q_final, tokens * static_cast<std::size_t>(q_dim), "q_final");
        !s.ok()) {
        return s;
    }
    if (const auto s = need(k_final, tokens * static_cast<std::size_t>(kv_dim), "k_final");
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rope(q_rope, q_final, seq * config_.n_heads, head_dim,
                                 config_.rope_base, 0, config_.n_heads);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rope(k_rope, k_final, seq * config_.n_kv_heads, head_dim,
                                 config_.rope_base, 0, config_.n_kv_heads);
        !s.ok()) {
        return s;
    }

    DeviceBuffer context, lse;
    if (const auto s = need(context, tokens * static_cast<std::size_t>(q_dim), "context");
        !s.ok()) {
        return s;
    }
    if (const auto s = need(lse, static_cast<std::size_t>(config_.n_heads) * tokens, "lse");
        !s.ok()) {
        return s;
    }
    DeviceBuffer scores;
    if (const auto s = need(scores, static_cast<std::size_t>(config_.n_heads) * tokens * tokens,
                            "scores");
        !s.ok()) {
        return s;
    }
    auto* cuda = dynamic_cast<CudaDevice*>(&device);
    if (cuda == nullptr) return ComputeStatus::unsupported("forward_layer_cached: needs CUDA");
    if (const auto s = cuda_causal_attention_batched(*cuda, q_final, k_final, v, scores, context,
                                                     lse, seq, config_.n_heads, config_.n_kv_heads,
                                                     head_dim);
        !s.ok()) {
        return s;
    }

    DeviceBuffer attention_out;
    if (const auto s = need(attention_out, hidden_elements, "attention_out"); !s.ok()) return s;
    if (const auto s = project(device, context, w_o.buffer(), attention_out, seq, q_dim, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = device.axpy(1.0, hidden.buffer(), attention_out); !s.ok()) return s;

    // --- MLP ---------------------------------------------------------------------------
    DeviceBuffer mlp_normed, gate, up, activated, mlp_out;
    if (const auto s = need(mlp_normed, hidden_elements, "mlp_normed"); !s.ok()) return s;
    if (const auto s = cuda_rms_norm(attention_out, post_norm.buffer(), mlp_normed, seq, d,
                                     config_.rms_eps);
        !s.ok()) {
        return s;
    }
    const std::size_t ff = tokens * static_cast<std::size_t>(config_.d_ff);
    if (const auto s = need(gate, ff, "gate"); !s.ok()) return s;
    if (const auto s = need(up, ff, "up"); !s.ok()) return s;
    if (const auto s = need(activated, ff, "activated"); !s.ok()) return s;
    if (const auto s = need(mlp_out, hidden_elements, "mlp_out"); !s.ok()) return s;
    if (const auto s = project(device, mlp_normed, w_gate.buffer(), gate, seq, d, config_.d_ff);
        !s.ok()) {
        return s;
    }
    if (const auto s = project(device, mlp_normed, w_up.buffer(), up, seq, d, config_.d_ff);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_swiglu(gate, up, activated, ff); !s.ok()) return s;
    if (const auto s = project(device, activated, w_down.buffer(), mlp_out, seq, config_.d_ff, d);
        !s.ok()) {
        return s;
    }

    // Retain the attention-block output BEFORE adding the MLP residual: the MLP's backward needs
    // it as the input to post_attention_layernorm.
    cache.after_attention = ResidentTensor::adopt(std::move(attention_out), {seq, d});
    if (const auto s = device.axpy(1.0, cache.after_attention.buffer(), mlp_out); !s.ok()) return s;

    cache.input = std::move(hidden);
    cache.normed = ResidentTensor::adopt(std::move(normed), {seq, d});
    cache.query = ResidentTensor::adopt(std::move(q_final), {seq, q_dim});
    cache.key = ResidentTensor::adopt(std::move(k_final), {seq, kv_dim});
    cache.query_pre = ResidentTensor::adopt(std::move(q), {seq, q_dim});
    cache.key_pre = ResidentTensor::adopt(std::move(k), {seq, kv_dim});
    cache.query_normed = ResidentTensor::adopt(std::move(q_rope), {seq, q_dim});
    cache.key_normed = ResidentTensor::adopt(std::move(k_rope), {seq, kv_dim});
    cache.value = ResidentTensor::adopt(std::move(v), {seq, kv_dim});  // no norm/rope on V
    cache.softmax_lse = ResidentTensor::adopt(std::move(lse), {config_.n_heads, seq});
    cache.probabilities = ResidentTensor::adopt(std::move(scores), {config_.n_heads * seq, seq});
    cache.context = ResidentTensor::adopt(std::move(context), {seq, q_dim});
    cache.mlp_normed = ResidentTensor::adopt(std::move(mlp_normed), {seq, d});
    cache.gate = ResidentTensor::adopt(std::move(gate), {seq, config_.d_ff});
    cache.up = ResidentTensor::adopt(std::move(up), {seq, config_.d_ff});
    cache.activated = ResidentTensor::adopt(std::move(activated), {seq, config_.d_ff});
    out = ResidentTensor::adopt(std::move(mlp_out), {seq, d});
    return ComputeStatus::success();
}

ComputeStatus CudaQwenModel::forward_layer(ComputeDevice& device, const ResidentTensor& hidden,
                                           int layer, int seq, ResidentTensor& out) const {
    if (!valid()) return ComputeStatus::invalid_argument("forward_layer: model is not resident");
    if (layer < 0 || layer >= config_.n_layers) {
        return ComputeStatus::invalid_argument("forward_layer: layer out of range");
    }
    const int d = config_.d_model;
    const int head_dim = config_.head_dim;
    const int q_dim = config_.n_heads * head_dim;
    const int kv_dim = config_.n_kv_heads * head_dim;
    const std::size_t tokens = static_cast<std::size_t>(seq);
    const std::size_t hidden_elements = tokens * static_cast<std::size_t>(d);

    const std::size_t base = layer_base(layer);
    const auto& input_norm = parameters_[base + 0];
    const auto& w_q = parameters_[base + 1];
    const auto& w_k = parameters_[base + 2];
    const auto& w_v = parameters_[base + 3];
    const auto& w_o = parameters_[base + 4];
    const auto& q_norm = parameters_[base + 5];
    const auto& k_norm = parameters_[base + 6];
    const auto& post_norm = parameters_[base + 7];
    const auto& w_gate = parameters_[base + 8];
    const auto& w_up = parameters_[base + 9];
    const auto& w_down = parameters_[base + 10];

    // --- attention block ------------------------------------------------------------------
    DeviceBuffer normed = scratch(device, hidden_elements);
    if (normed.empty()) return ComputeStatus::out_of_memory("forward_layer: normed");
    if (const auto status = cuda_rms_norm(hidden.buffer(), input_norm.buffer(), normed, seq, d,
                                          config_.rms_eps);
        !status.ok()) {
        return status;
    }

    DeviceBuffer q = scratch(device, tokens * static_cast<std::size_t>(q_dim));
    DeviceBuffer k = scratch(device, tokens * static_cast<std::size_t>(kv_dim));
    DeviceBuffer v = scratch(device, tokens * static_cast<std::size_t>(kv_dim));
    if (q.empty() || k.empty() || v.empty()) {
        return ComputeStatus::out_of_memory("forward_layer: qkv");
    }
    if (const auto s = project(device, normed, w_q.buffer(), q, seq, d, q_dim); !s.ok()) return s;
    if (const auto s = project(device, normed, w_k.buffer(), k, seq, d, kv_dim); !s.ok()) return s;
    if (const auto s = project(device, normed, w_v.buffer(), v, seq, d, kv_dim); !s.ok()) return s;

    // QK-norm then RoPE. [seq, n_heads * head_dim] reshapes CONTIGUOUSLY to
    // [seq * n_heads, head_dim], so both run over the packed layout with no copy; RoPE recovers
    // the position as row / n_heads (rung 3a).
    DeviceBuffer q_normed = scratch(device, tokens * static_cast<std::size_t>(q_dim));
    DeviceBuffer k_normed = scratch(device, tokens * static_cast<std::size_t>(kv_dim));
    if (q_normed.empty() || k_normed.empty()) {
        return ComputeStatus::out_of_memory("forward_layer: qk norm");
    }
    if (const auto s = cuda_rms_norm(q, q_norm.buffer(), q_normed, seq * config_.n_heads, head_dim,
                                     config_.rms_eps);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rms_norm(k, k_norm.buffer(), k_normed, seq * config_.n_kv_heads,
                                     head_dim, config_.rms_eps);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rope(q_normed, q, seq * config_.n_heads, head_dim, config_.rope_base, 0,
                                 config_.n_heads);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rope(k_normed, k, seq * config_.n_kv_heads, head_dim,
                                 config_.rope_base, 0, config_.n_kv_heads);
        !s.ok()) {
        return s;
    }

    DeviceBuffer context = scratch(device, tokens * static_cast<std::size_t>(q_dim));
    DeviceBuffer lse = scratch(device, static_cast<std::size_t>(config_.n_heads) * tokens);
    if (context.empty() || lse.empty()) {
        return ComputeStatus::out_of_memory("forward_layer: attention");
    }
    // Batched GEMM on the tensor cores rather than the streaming kernel: at real sequence lengths
    // the serial key walk dominates, and QK^T / PV are matmuls that belong on tensor cores. Costs a
    // [n_heads, seq, seq] scratch, freed with the layer.
    DeviceBuffer scores = scratch(device, static_cast<std::size_t>(config_.n_heads) * tokens * tokens);
    if (scores.empty()) return ComputeStatus::out_of_memory("forward_layer: scores");
    auto* cuda = dynamic_cast<CudaDevice*>(&device);
    if (cuda == nullptr) return ComputeStatus::unsupported("forward_layer: needs a CUDA device");
    if (const auto s = cuda_causal_attention_batched(*cuda, q, k, v, scores, context, lse, seq,
                                                     config_.n_heads, config_.n_kv_heads, head_dim);
        !s.ok()) {
        return s;
    }

    DeviceBuffer attention_out = scratch(device, hidden_elements);
    if (attention_out.empty()) return ComputeStatus::out_of_memory("forward_layer: o_proj");
    if (const auto s = project(device, context, w_o.buffer(), attention_out, seq, q_dim, d);
        !s.ok()) {
        return s;
    }
    // Residual: h = hidden + attention_out, accumulated in place via axpy (y += 1.0 * x).
    if (const auto s = device.axpy(1.0, hidden.buffer(), attention_out); !s.ok()) return s;

    // --- MLP block ------------------------------------------------------------------------
    DeviceBuffer mlp_normed = scratch(device, hidden_elements);
    if (mlp_normed.empty()) return ComputeStatus::out_of_memory("forward_layer: mlp norm");
    if (const auto s = cuda_rms_norm(attention_out, post_norm.buffer(), mlp_normed, seq, d,
                                     config_.rms_eps);
        !s.ok()) {
        return s;
    }

    const std::size_t ff_elements = tokens * static_cast<std::size_t>(config_.d_ff);
    DeviceBuffer gate = scratch(device, ff_elements);
    DeviceBuffer up = scratch(device, ff_elements);
    DeviceBuffer activated = scratch(device, ff_elements);
    if (gate.empty() || up.empty() || activated.empty()) {
        return ComputeStatus::out_of_memory("forward_layer: mlp");
    }
    if (const auto s = project(device, mlp_normed, w_gate.buffer(), gate, seq, d, config_.d_ff);
        !s.ok()) {
        return s;
    }
    if (const auto s = project(device, mlp_normed, w_up.buffer(), up, seq, d, config_.d_ff);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_swiglu(gate, up, activated, ff_elements); !s.ok()) return s;

    DeviceBuffer mlp_out = scratch(device, hidden_elements);
    if (mlp_out.empty()) return ComputeStatus::out_of_memory("forward_layer: down_proj");
    if (const auto s = project(device, activated, w_down.buffer(), mlp_out, seq, config_.d_ff, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = device.axpy(1.0, attention_out, mlp_out); !s.ok()) return s;

    out = ResidentTensor::adopt(std::move(mlp_out), {seq, d});
    return ComputeStatus::success();
}

std::size_t CudaQwenModel::resident_bytes() const noexcept {
    std::size_t total = 0;
    for (const ResidentTensor& parameter : parameters_) total += parameter.size_bytes();
    return total;
}


CudaQwenModel::Gradients CudaQwenModel::make_gradients(ComputeDevice& device) const {
    Gradients gradients;
    gradients.reserve(parameters_.size());
    for (const ResidentTensor& parameter : parameters_) {
        const std::vector<float> zeros(parameter.element_count(), 0.0F);
        gradients.push_back(ResidentTensor::upload(device, zeros, parameter.shape()));
    }
    return gradients;
}

ComputeStatus CudaQwenModel::zero_gradients(ComputeDevice& device, Gradients& gradients) const {
    if (gradients.size() != parameters_.size()) {
        return ComputeStatus::invalid_argument("zero_gradients: size mismatch");
    }
    for (std::size_t index = 0; index < gradients.size(); ++index) {
        // scale by 0 rather than reuploading zeros: a 2.4 GB memset from the host per step would
        // reintroduce exactly the PCIe traffic this whole design removes.
        if (const auto status = device.axpy(-1.0, gradients[index].buffer(),
                                            gradients[index].buffer());
            !status.ok()) {
            return status;
        }
    }
    return ComputeStatus::success();
}

ComputeStatus CudaQwenModel::backward_layer(ComputeDevice& device, const LayerCache& cache,
                                            const ResidentTensor& grad_out, int layer, int seq,
                                            Gradients& gradients,
                                            ResidentTensor& grad_hidden) const {
    if (!valid()) return ComputeStatus::invalid_argument("backward_layer: not resident");
    const int d = config_.d_model;
    const int head_dim = config_.head_dim;
    const int q_dim = config_.n_heads * head_dim;
    const int kv_dim = config_.n_kv_heads * head_dim;
    const std::size_t tokens = static_cast<std::size_t>(seq);
    const std::size_t hidden_elements = tokens * static_cast<std::size_t>(d);
    const std::size_t ff = tokens * static_cast<std::size_t>(config_.d_ff);

    const std::size_t base = layer_base(layer);
    const auto& w_q = parameters_[base + 1];
    const auto& w_k = parameters_[base + 2];
    const auto& w_v = parameters_[base + 3];
    const auto& w_o = parameters_[base + 4];
    const auto& w_gate = parameters_[base + 8];
    const auto& w_up = parameters_[base + 9];
    const auto& w_down = parameters_[base + 10];

    const auto need = [&](DeviceBuffer& buffer, std::size_t elements, const char* what) {
        buffer = device.allocate(DataType::Float32, elements);
        return buffer.empty() ? ComputeStatus::out_of_memory(std::string("backward: ") + what)
                              : ComputeStatus::success();
    };

    // --- MLP block, in reverse -----------------------------------------------------------
    // down_proj
    DeviceBuffer grad_activated;
    if (const auto s = need(grad_activated, ff, "grad_activated"); !s.ok()) return s;
    if (const auto s = input_grad(device, grad_out.buffer(), w_down.buffer(), grad_activated, seq,
                                  d, config_.d_ff);
        !s.ok()) {
        return s;
    }
    if (const auto s = accumulate_weight_grad(device, grad_out.buffer(), cache.activated.buffer(),
                                              gradients[base + 10].buffer(), seq, d, config_.d_ff);
        !s.ok()) {
        return s;
    }

    // swiglu
    DeviceBuffer grad_gate, grad_up;
    if (const auto s = need(grad_gate, ff, "grad_gate"); !s.ok()) return s;
    if (const auto s = need(grad_up, ff, "grad_up"); !s.ok()) return s;
    if (const auto s = cuda_swiglu_backward(cache.gate.buffer(), cache.up.buffer(), grad_activated,
                                            grad_gate, grad_up, ff);
        !s.ok()) {
        return s;
    }

    // gate_proj and up_proj both read mlp_normed, so their input gradients SUM.
    DeviceBuffer grad_mlp_normed, grad_from_up;
    if (const auto s = need(grad_mlp_normed, hidden_elements, "grad_mlp_normed"); !s.ok()) return s;
    if (const auto s = need(grad_from_up, hidden_elements, "grad_from_up"); !s.ok()) return s;
    if (const auto s = input_grad(device, grad_gate, w_gate.buffer(), grad_mlp_normed, seq,
                                  config_.d_ff, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = input_grad(device, grad_up, w_up.buffer(), grad_from_up, seq, config_.d_ff,
                                  d);
        !s.ok()) {
        return s;
    }
    if (const auto s = device.axpy(1.0, grad_from_up, grad_mlp_normed); !s.ok()) return s;
    if (const auto s = accumulate_weight_grad(device, grad_gate, cache.mlp_normed.buffer(),
                                              gradients[base + 8].buffer(), seq, config_.d_ff, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = accumulate_weight_grad(device, grad_up, cache.mlp_normed.buffer(),
                                              gradients[base + 9].buffer(), seq, config_.d_ff, d);
        !s.ok()) {
        return s;
    }

    // post_attention_layernorm
    DeviceBuffer grad_after_attention;
    if (const auto s = need(grad_after_attention, hidden_elements, "grad_after_attention");
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rms_norm_backward(
            cache.after_attention.buffer(), parameters_[base + 7].buffer(), grad_mlp_normed,
            grad_after_attention, gradients[base + 7].buffer(), seq, d, config_.rms_eps);
        !s.ok()) {
        return s;
    }
    // The MLP residual: h_out = after_attention + mlp_out, so grad flows both ways.
    if (const auto s = device.axpy(1.0, grad_out.buffer(), grad_after_attention); !s.ok()) return s;

    // --- attention block, in reverse -----------------------------------------------------
    DeviceBuffer grad_context;
    if (const auto s = need(grad_context, tokens * static_cast<std::size_t>(q_dim), "grad_context");
        !s.ok()) {
        return s;
    }
    if (const auto s = input_grad(device, grad_after_attention, w_o.buffer(), grad_context, seq, d,
                                  q_dim);
        !s.ok()) {
        return s;
    }
    if (const auto s = accumulate_weight_grad(device, grad_after_attention, cache.context.buffer(),
                                              gradients[base + 4].buffer(), seq, d, q_dim);
        !s.ok()) {
        return s;
    }

    DeviceBuffer grad_q, grad_k, grad_v;
    if (const auto s = need(grad_q, tokens * static_cast<std::size_t>(q_dim), "grad_q"); !s.ok()) {
        return s;
    }
    if (const auto s = need(grad_k, tokens * static_cast<std::size_t>(kv_dim), "grad_k"); !s.ok()) {
        return s;
    }
    if (const auto s = need(grad_v, tokens * static_cast<std::size_t>(kv_dim), "grad_v"); !s.ok()) {
        return s;
    }
    DeviceBuffer grad_scores;
    if (const auto s = need(grad_scores, static_cast<std::size_t>(config_.n_heads) * tokens * tokens,
                            "grad_scores");
        !s.ok()) {
        return s;
    }
    auto* cuda = dynamic_cast<CudaDevice*>(&device);
    if (cuda == nullptr) return ComputeStatus::unsupported("backward_layer: needs a CUDA device");
    if (const auto s = cuda_causal_attention_backward_batched(
            *cuda, cache.query.buffer(), cache.key.buffer(), cache.value.buffer(),
            cache.probabilities.buffer(), grad_context, grad_scores, grad_q, grad_k, grad_v, seq,
            config_.n_heads, config_.n_kv_heads, head_dim);
        !s.ok()) {
        return s;
    }

    // RoPE is a rotation, so its backward is the inverse rotation — the same kernel, inverted.
    DeviceBuffer grad_q_normed, grad_k_normed;
    if (const auto s = need(grad_q_normed, tokens * static_cast<std::size_t>(q_dim), "grad_q_rope");
        !s.ok()) {
        return s;
    }
    if (const auto s = need(grad_k_normed, tokens * static_cast<std::size_t>(kv_dim), "grad_k_rope");
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rope_backward(grad_q, grad_q_normed, seq * config_.n_heads, head_dim,
                                          config_.rope_base, 0, config_.n_heads);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rope_backward(grad_k, grad_k_normed, seq * config_.n_kv_heads, head_dim,
                                          config_.rope_base, 0, config_.n_kv_heads);
        !s.ok()) {
        return s;
    }

    // QK-norm backward, over the packed [seq * heads, head_dim] view.
    DeviceBuffer grad_q_pre, grad_k_pre;
    if (const auto s = need(grad_q_pre, tokens * static_cast<std::size_t>(q_dim), "grad_q_pre");
        !s.ok()) {
        return s;
    }
    if (const auto s = need(grad_k_pre, tokens * static_cast<std::size_t>(kv_dim), "grad_k_pre");
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rms_norm_backward(
            cache.query_pre.buffer(), parameters_[base + 5].buffer(), grad_q_normed, grad_q_pre,
            gradients[base + 5].buffer(), seq * config_.n_heads, head_dim, config_.rms_eps);
        !s.ok()) {
        return s;
    }
    if (const auto s = cuda_rms_norm_backward(
            cache.key_pre.buffer(), parameters_[base + 6].buffer(), grad_k_normed, grad_k_pre,
            gradients[base + 6].buffer(), seq * config_.n_kv_heads, head_dim, config_.rms_eps);
        !s.ok()) {
        return s;
    }

    // q/k/v projections all read `normed`, so their input gradients sum.
    DeviceBuffer grad_normed, grad_scratch;
    if (const auto s = need(grad_normed, hidden_elements, "grad_normed"); !s.ok()) return s;
    if (const auto s = need(grad_scratch, hidden_elements, "grad_scratch"); !s.ok()) return s;
    if (const auto s = input_grad(device, grad_q_pre, w_q.buffer(), grad_normed, seq, q_dim, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = input_grad(device, grad_k_pre, w_k.buffer(), grad_scratch, seq, kv_dim, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = device.axpy(1.0, grad_scratch, grad_normed); !s.ok()) return s;
    if (const auto s = input_grad(device, grad_v, w_v.buffer(), grad_scratch, seq, kv_dim, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = device.axpy(1.0, grad_scratch, grad_normed); !s.ok()) return s;

    if (const auto s = accumulate_weight_grad(device, grad_q_pre, cache.normed.buffer(),
                                              gradients[base + 1].buffer(), seq, q_dim, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = accumulate_weight_grad(device, grad_k_pre, cache.normed.buffer(),
                                              gradients[base + 2].buffer(), seq, kv_dim, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = accumulate_weight_grad(device, grad_v, cache.normed.buffer(),
                                              gradients[base + 3].buffer(), seq, kv_dim, d);
        !s.ok()) {
        return s;
    }

    // input_layernorm, then the attention residual.
    DeviceBuffer grad_in;
    if (const auto s = need(grad_in, hidden_elements, "grad_in"); !s.ok()) return s;
    if (const auto s = cuda_rms_norm_backward(cache.input.buffer(), parameters_[base + 0].buffer(),
                                              grad_normed, grad_in, gradients[base + 0].buffer(),
                                              seq, d, config_.rms_eps);
        !s.ok()) {
        return s;
    }
    if (const auto s = device.axpy(1.0, grad_after_attention, grad_in); !s.ok()) return s;

    grad_hidden = ResidentTensor::adopt(std::move(grad_in), {seq, d});
    return ComputeStatus::success();
}


ComputeStatus CudaQwenModel::train_step(ComputeDevice& device, const std::vector<int>& ids,
                                        const std::vector<int>& targets, Gradients& gradients,
                                        Gradients& moment1, Gradients& moment2, int step,
                                        float learning_rate, float& loss) {
    if (!valid()) return ComputeStatus::invalid_argument("train_step: model is not resident");
    if (ids.empty() || targets.size() + 1 != ids.size()) {
        // Next-token prediction: position t predicts token t+1, so targets is exactly one shorter.
        return ComputeStatus::invalid_argument("train_step: targets must be ids shifted by one");
    }
    const int seq = static_cast<int>(ids.size());
    const int predicted = seq - 1;  // the last position has no next token to predict
    const int d = config_.d_model;
    const int vocab = config_.vocab_size;
    const std::size_t hidden_elements =
        static_cast<std::size_t>(seq) * static_cast<std::size_t>(d);

    if (const auto s = zero_gradients(device, gradients); !s.ok()) return s;

    // --- forward, retaining every layer's cache -------------------------------------------
    DeviceBuffer device_ids = device.upload<int>(std::span<const int>(ids));
    DeviceBuffer device_targets = device.upload<int>(std::span<const int>(targets));
    if (device_ids.empty() || device_targets.empty()) {
        return ComputeStatus::out_of_memory("train_step: ids");
    }
    DeviceBuffer embedded = device.allocate(DataType::Float32, hidden_elements);
    if (embedded.empty()) return ComputeStatus::out_of_memory("train_step: embedding");
    if (const auto s = cuda_embedding(parameters_[0].buffer(), device_ids, embedded, seq, d, vocab);
        !s.ok()) {
        return s;
    }

    ResidentTensor hidden = ResidentTensor::adopt(std::move(embedded), {seq, d});
    std::vector<LayerCache> caches(static_cast<std::size_t>(config_.n_layers));
    for (int layer = 0; layer < config_.n_layers; ++layer) {
        ResidentTensor next;
        if (const auto s = forward_layer_cached(device, std::move(hidden), layer, seq,
                                                caches[static_cast<std::size_t>(layer)], next);
            !s.ok()) {
            return s;
        }
        hidden = std::move(next);
    }

    const std::size_t final_norm_index =
        1 + static_cast<std::size_t>(config_.n_layers) * kPerLayer;
    DeviceBuffer normed = device.allocate(DataType::Float32, hidden_elements);
    if (normed.empty()) return ComputeStatus::out_of_memory("train_step: final norm");
    if (const auto s = cuda_rms_norm(hidden.buffer(), parameters_[final_norm_index].buffer(),
                                     normed, seq, d, config_.rms_eps);
        !s.ok()) {
        return s;
    }
    const std::size_t output_index =
        config_.tie_word_embeddings ? 0 : final_norm_index + 1;

    DeviceBuffer logits = device.allocate(
        DataType::Float32, static_cast<std::size_t>(seq) * static_cast<std::size_t>(vocab));
    if (logits.empty()) return ComputeStatus::out_of_memory("train_step: logits");
    if (const auto s = project(device, normed, parameters_[output_index].buffer(), logits, seq, d,
                               vocab);
        !s.ok()) {
        return s;
    }

    // --- loss over the predicted positions -------------------------------------------------
    DeviceBuffer row_loss = device.allocate(DataType::Float32, static_cast<std::size_t>(predicted));
    DeviceBuffer total = device.allocate(DataType::Float32, 1);
    if (row_loss.empty() || total.empty()) return ComputeStatus::out_of_memory("train_step: loss");
    if (const auto s = cuda_cross_entropy(logits, device_targets, row_loss, total, predicted, vocab);
        !s.ok()) {
        return s;
    }
    // The ONLY value that crosses PCIe on the way out: one float.
    std::vector<float> host_loss(1, 0.0F);
    if (const auto s = device.download<float>(total, std::span<float>(host_loss)); !s.ok()) return s;
    loss = host_loss[0];

    // --- backward ---------------------------------------------------------------------------
    DeviceBuffer grad_logits = device.allocate(
        DataType::Float32, static_cast<std::size_t>(seq) * static_cast<std::size_t>(vocab));
    if (grad_logits.empty()) return ComputeStatus::out_of_memory("train_step: grad_logits");
    // Zero it: only the predicted rows receive a gradient, and the final position must contribute
    // nothing rather than whatever the allocator last left there.
    if (const auto s = device.axpy(-1.0, grad_logits, grad_logits); !s.ok()) return s;
    if (const auto s = cuda_cross_entropy_backward(logits, device_targets, grad_logits, predicted,
                                                   vocab);
        !s.ok()) {
        return s;
    }

    DeviceBuffer grad_normed = device.allocate(DataType::Float32, hidden_elements);
    if (grad_normed.empty()) return ComputeStatus::out_of_memory("train_step: grad_normed");
    if (const auto s = input_grad(device, grad_logits, parameters_[output_index].buffer(),
                                  grad_normed, seq, vocab, d);
        !s.ok()) {
        return s;
    }
    if (const auto s = accumulate_weight_grad(device, grad_logits, normed,
                                              gradients[output_index].buffer(), seq, vocab, d);
        !s.ok()) {
        return s;
    }

    DeviceBuffer grad_hidden_buffer = device.allocate(DataType::Float32, hidden_elements);
    if (grad_hidden_buffer.empty()) return ComputeStatus::out_of_memory("train_step: grad_hidden");
    if (const auto s = cuda_rms_norm_backward(
            hidden.buffer(), parameters_[final_norm_index].buffer(), grad_normed,
            grad_hidden_buffer, gradients[final_norm_index].buffer(), seq, d, config_.rms_eps);
        !s.ok()) {
        return s;
    }

    ResidentTensor grad_hidden = ResidentTensor::adopt(std::move(grad_hidden_buffer), {seq, d});
    for (int layer = config_.n_layers - 1; layer >= 0; --layer) {
        ResidentTensor next;
        if (const auto s = backward_layer(device, caches[static_cast<std::size_t>(layer)],
                                          grad_hidden, layer, seq, gradients, next);
            !s.ok()) {
            return s;
        }
        grad_hidden = std::move(next);
    }
    if (const auto s = cuda_embedding_backward(grad_hidden.buffer(), device_ids,
                                               gradients[0].buffer(), seq, d, vocab);
        !s.ok()) {
        return s;
    }

    // --- AdamW ------------------------------------------------------------------------------
    for (std::size_t index = 0; index < parameters_.size(); ++index) {
        if (const auto s = cuda_adamw_step(parameters_[index].buffer(), gradients[index].buffer(),
                                           moment1[index].buffer(), moment2[index].buffer(),
                                           parameters_[index].element_count(), learning_rate, 0.9F,
                                           0.999F, 1e-8F, 0.0F, step);
            !s.ok()) {
            return s;
        }
    }
    return ComputeStatus::success();
}

ComputeStatus CudaQwenModel::download_into(zetta::autograd::QwenModel& host) const {
    if (parameters_.size() != host.parameters().size()) {
        return ComputeStatus::invalid_argument("download_into: parameter count mismatch");
    }
    for (std::size_t index = 0; index < parameters_.size(); ++index) {
        std::vector<float> values(parameters_[index].element_count(), 0.0F);
        if (const auto s = parameters_[index].download(std::span<float>(values)); !s.ok()) return s;
        host.parameters()[index] = zetta::autograd::Tensor::leaf(
            std::move(values), parameters_[index].shape(), /*requires_grad=*/true);
    }
    return ComputeStatus::success();
}

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
