#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#if defined(ZETTA_WITH_CUDA)
#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/autograd/transformer.hpp"
#include "zetta/compute/cuda_resident.hpp"
#include "zetta/compute/cuda_device.hpp"
#include "zetta/compute/cuda_qwen_model.hpp"
#endif

// Rung 2 of #923: every weight uploaded ONCE and staying there.
//
// This is the measurable fix for what #928 recorded — 425 MiB resident of 40,960 MiB while training
// a 0.6B model, because `device_gemm` re-uploads its operands on every call. After this rung the
// resident bytes must account for the whole parameter set, and the values must be exactly what the
// CPU loader produced: residency is worthless if the weights arrive wrong.
namespace {

#if defined(ZETTA_WITH_CUDA)

using zetta::autograd::QwenConfig;
using zetta::autograd::QwenModel;
using zetta::compute::CudaDevice;
using zetta::compute::CudaQwenModel;

bool cuda_present(CudaDevice& device) {
    const auto info = device.info();
    return !(info.name.empty() || info.name == "cuda");
}

// Tiny but architecturally real: GQA (4 query heads over 2 kv heads) and more than one layer, so
// per-layer and per-head indexing is exercised rather than trivially correct.
QwenConfig tiny_config() {
    QwenConfig config;
    config.vocab_size = 64;
    config.d_model = 16;
    config.n_layers = 2;
    config.n_heads = 4;
    config.n_kv_heads = 2;
    config.head_dim = 4;
    config.d_ff = 32;
    return config;
}

TEST(CudaQwenModel, UploadsEveryParameterOnceAndKeepsItResident) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    QwenModel host(tiny_config(), /*seed=*/7);
    const std::size_t expected_parameters = host.parameters().size();
    std::size_t expected_bytes = 0;
    for (const auto& parameter : host.parameters()) {
        expected_bytes += parameter.data().size() * sizeof(float);
    }

    const std::size_t resident_before = device.pooled_bytes();  // pool must not be confused for it
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());

    EXPECT_EQ(resident.parameter_count(), expected_parameters);
    // The whole parameter set is on the card — this is the number that was 425 MiB of 40,960.
    EXPECT_EQ(resident.resident_bytes(), expected_bytes);
    EXPECT_EQ(device.pooled_bytes(), resident_before) << "residency must not come from the pool";
}

TEST(CudaQwenModel, ResidentWeightsMatchTheHostBitForBit) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    QwenModel host(tiny_config(), /*seed=*/11);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());
    ASSERT_EQ(resident.parameter_count(), host.parameters().size());

    // Bit-for-bit, not approximately: an upload is a copy, so any difference here is a real bug
    // (a wrong offset, a truncated length) rather than a numerical tolerance question.
    for (std::size_t index = 0; index < resident.parameter_count(); ++index) {
        const std::vector<float>& expected = host.parameters()[index].data();
        std::vector<float> actual(expected.size(), 0.0F);
        ASSERT_TRUE(resident.parameter(index).download(std::span<float>(actual)).ok())
            << "parameter " << index;
        EXPECT_EQ(actual, expected) << "parameter " << index << " differs after upload";
    }
}

TEST(CudaQwenModel, ParameterShapesSurviveTheUpload) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    QwenModel host(tiny_config(), /*seed=*/13);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());
    // The kernels are launched from these shapes; a lost or reordered shape would be a
    // out-of-bounds launch reading as corrupt numbers rather than an error.
    for (std::size_t index = 0; index < resident.parameter_count(); ++index) {
        EXPECT_EQ(resident.parameter(index).shape(), host.parameters()[index].shape())
            << "parameter " << index;
    }
}


// Rung 3 of #923: one decoder layer, entirely on-device.
//
// This is where the #922 kernels stop being shelfware. The layer composes rms_norm -> QK-norm ->
// RoPE -> causal attention -> SwiGLU with cuBLAS projections, all over resident storage: the only
// host traffic is the input going in and the output coming out. Correctness is judged against the
// autograd engine, which is the reference implementation every other backend is checked against.
TEST(CudaQwenModel, OneDecoderLayerMatchesTheAutogradReference) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const QwenConfig config = tiny_config();
    QwenModel host(config, /*seed=*/17);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());

    // A short token sequence; every position attends causally over the ones before it.
    const std::vector<int> ids = {5, 9, 2, 14, 7};
    const int seq = static_cast<int>(ids.size());

    // Reference: run the host model's own embedding + first layer.
    const zetta::autograd::Tensor host_hidden =
        zetta::autograd::embedding(host.weight("model.embed_tokens.weight"), ids);
    // Composed from the public autograd ops rather than a test-only hook on QwenModel, mirroring
    // QwenModel::forward exactly: pre-norm residual attention, then pre-norm residual SwiGLU MLP.
    const std::string prefix = "model.layers.0.";
    const zetta::autograd::Tensor attn_in = zetta::autograd::rms_norm(
        host_hidden, host.weight(prefix + "input_layernorm.weight"), config.rms_eps);
    const zetta::autograd::Tensor attn = zetta::autograd::grouped_query_attention(
        attn_in, host.weight(prefix + "self_attn.q_proj.weight"),
        host.weight(prefix + "self_attn.k_proj.weight"),
        host.weight(prefix + "self_attn.v_proj.weight"),
        host.weight(prefix + "self_attn.o_proj.weight"),
        host.weight(prefix + "self_attn.q_norm.weight"),
        host.weight(prefix + "self_attn.k_norm.weight"), config.n_heads, config.n_kv_heads,
        config.head_dim, config.rope_base);
    const zetta::autograd::Tensor after_attention = zetta::autograd::add(host_hidden, attn);
    const zetta::autograd::Tensor mlp_in = zetta::autograd::rms_norm(
        after_attention, host.weight(prefix + "post_attention_layernorm.weight"), config.rms_eps);
    const zetta::autograd::Tensor mlp = zetta::autograd::swiglu_feed_forward(
        mlp_in, host.weight(prefix + "mlp.gate_proj.weight"),
        host.weight(prefix + "mlp.up_proj.weight"), host.weight(prefix + "mlp.down_proj.weight"));
    const zetta::autograd::Tensor host_out = zetta::autograd::add(after_attention, mlp);

    // Device: upload the same hidden state once, run the layer, bring only the result back.
    zetta::compute::ResidentTensor hidden = zetta::compute::ResidentTensor::upload(
        device, host_hidden.data(), host_hidden.shape());
    ASSERT_TRUE(hidden.valid());
    zetta::compute::ResidentTensor device_out;
    const auto status = resident.forward_layer(device, hidden, /*layer=*/0, seq, device_out);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_TRUE(device_out.valid());

    std::vector<float> actual(host_out.data().size(), 0.0F);
    ASSERT_TRUE(device_out.download(std::span<float>(actual)).ok());
    ASSERT_EQ(actual.size(), host_out.data().size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        // fp32 tolerance, not bit-equality: cuBLAS reassociates and TF32 rounds the mantissa, so
        // demanding exact agreement here would be demanding the two backends compute identically
        // rather than correctly.
        EXPECT_NEAR(actual[index], host_out.data()[index], 2e-3F) << "at " << index;
    }
}

#endif  // ZETTA_WITH_CUDA

}  // namespace
