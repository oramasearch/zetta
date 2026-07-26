#include "zetta/compute/cuda_qwen_model.hpp"

#include <span>
#include <utility>

#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"
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
    if (const auto s = cuda_causal_attention(q, k, v, context, lse, seq, config_.n_heads,
                                             config_.n_kv_heads, head_dim);
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

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
