#include <gtest/gtest.h>

#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <iostream>
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


// Rung 4 of #923: the whole forward on-device — embed, every layer, the final norm, the output
// projection. Only the logits come back. If this matches the autograd engine, the model runs on the
// GPU; everything after this rung is training machinery on top of a proven forward.
TEST(CudaQwenModel, FullForwardMatchesTheAutogradReference) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const QwenConfig config = tiny_config();
    QwenModel host(config, /*seed=*/23);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());

    const std::vector<int> ids = {3, 11, 0, 27, 8, 19};
    const zetta::autograd::Tensor expected = host.forward(ids);

    zetta::compute::ResidentTensor logits;
    const auto status = resident.forward(device, ids, logits);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_TRUE(logits.valid());
    EXPECT_EQ(logits.shape(),
              (std::vector<int>{static_cast<int>(ids.size()), config.vocab_size}));

    std::vector<float> actual(expected.data().size(), 0.0F);
    ASSERT_TRUE(logits.download(std::span<float>(actual)).ok());
    ASSERT_EQ(actual.size(), expected.data().size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        // Tolerance grows through a multi-layer stack: cuBLAS reassociates and TF32 rounds the
        // mantissa, and those differences compound layer over layer. The check that matters is
        // that they stay bounded rather than diverging.
        EXPECT_NEAR(actual[index], expected.data()[index], 5e-3F) << "logit " << index;
    }
}

TEST(CudaQwenModel, ForwardIsDeterministicAcrossRuns) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";
    QwenModel host(tiny_config(), /*seed=*/29);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    const std::vector<int> ids = {1, 2, 3, 4};

    const auto run = [&] {
        zetta::compute::ResidentTensor logits;
        EXPECT_TRUE(resident.forward(device, ids, logits).ok());
        std::vector<float> values(static_cast<std::size_t>(ids.size()) * tiny_config().vocab_size,
                                  0.0F);
        EXPECT_TRUE(logits.download(std::span<float>(values)).ok());
        return values;
    };
    // Bitwise, not approximate: same inputs on the same device must give the same bits, or the
    // equivalence gate (#926) and every reproducibility claim built on it are meaningless.
    EXPECT_EQ(run(), run());
}


// Rung 6a: the cached forward must be the SAME forward. If the two diverge, every gradient built on
// the cache is a gradient of a function nobody runs.
TEST(CudaQwenModel, CachedLayerMatchesTheUncachedLayerAndFillsTheCache) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const QwenConfig config = tiny_config();
    QwenModel host(config, /*seed=*/31);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());

    const std::vector<int> ids = {2, 6, 1, 9};
    const int seq = static_cast<int>(ids.size());
    const zetta::autograd::Tensor host_hidden =
        zetta::autograd::embedding(host.weight("model.embed_tokens.weight"), ids);

    const auto upload_hidden = [&] {
        return zetta::compute::ResidentTensor::upload(device, host_hidden.data(),
                                                      host_hidden.shape());
    };

    zetta::compute::ResidentTensor plain_out;
    ASSERT_TRUE(resident.forward_layer(device, upload_hidden(), 0, seq, plain_out).ok());

    CudaQwenModel::LayerCache cache;
    zetta::compute::ResidentTensor cached_out;
    ASSERT_TRUE(
        resident.forward_layer_cached(device, upload_hidden(), 0, seq, cache, cached_out).ok());

    std::vector<float> plain(host_hidden.data().size(), 0.0F);
    std::vector<float> cached(host_hidden.data().size(), 0.0F);
    ASSERT_TRUE(plain_out.download(std::span<float>(plain)).ok());
    ASSERT_TRUE(cached_out.download(std::span<float>(cached)).ok());
    EXPECT_EQ(plain, cached) << "caching must not change the computation";

    // Every tensor the backward will consume is present and correctly shaped.
    EXPECT_TRUE(cache.input.valid());
    EXPECT_TRUE(cache.normed.valid());
    EXPECT_TRUE(cache.query.valid());
    EXPECT_TRUE(cache.key.valid());
    EXPECT_TRUE(cache.value.valid());
    EXPECT_TRUE(cache.softmax_lse.valid());
    EXPECT_TRUE(cache.context.valid());
    EXPECT_TRUE(cache.after_attention.valid());
    EXPECT_TRUE(cache.mlp_normed.valid());
    EXPECT_TRUE(cache.gate.valid());
    EXPECT_TRUE(cache.up.valid());
    EXPECT_TRUE(cache.activated.valid());
    EXPECT_EQ(cache.softmax_lse.shape(), (std::vector<int>{config.n_heads, seq}));
    EXPECT_EQ(cache.activated.shape(), (std::vector<int>{seq, config.d_ff}));
}


// Rung 6b: the layer's backward, gradient-checked.
//
// This is the only test that distinguishes a CORRECT backward from a merely self-consistent one. A
// backward can agree with itself, chain cleanly, and still not be the derivative of the forward —
// which in training shows up as "it learns, just worse", the hardest failure to attribute. Perturb
// an input, measure the forward's response, compare.
TEST(CudaQwenModel, LayerBackwardMatchesNumericalGradients) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const QwenConfig config = tiny_config();
    QwenModel host(config, /*seed=*/37);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());

    const int seq = 3;
    const int d = config.d_model;
    const std::size_t elements = static_cast<std::size_t>(seq) * d;
    std::vector<float> hidden(elements, 0.0F);
    for (std::size_t i = 0; i < elements; ++i) {
        hidden[i] = static_cast<float>((i * 37) % 19) / 19.0F - 0.5F;
    }
    // A non-uniform upstream gradient: an all-ones one can hide a term that is wrong only where
    // the upstream gradient varies.
    std::vector<float> upstream(elements, 0.0F);
    for (std::size_t i = 0; i < elements; ++i) {
        upstream[i] = static_cast<float>((i * 23) % 11) / 11.0F - 0.4F;
    }

    // Scalar objective L = sum(layer(hidden) * upstream), whose dL/dhidden the backward returns.
    const auto loss_for = [&](const std::vector<float>& input) {
        zetta::compute::ResidentTensor in =
            zetta::compute::ResidentTensor::upload(device, input, {seq, d});
        zetta::compute::ResidentTensor out;
        EXPECT_TRUE(resident.forward_layer(device, in, 0, seq, out).ok());
        std::vector<float> values(elements, 0.0F);
        EXPECT_TRUE(out.download(std::span<float>(values)).ok());
        double total = 0.0;
        for (std::size_t i = 0; i < elements; ++i) total += static_cast<double>(values[i]) * upstream[i];
        return total;
    };

    CudaQwenModel::LayerCache cache;
    zetta::compute::ResidentTensor out;
    ASSERT_TRUE(resident
                    .forward_layer_cached(
                        device, zetta::compute::ResidentTensor::upload(device, hidden, {seq, d}), 0,
                        seq, cache, out)
                    .ok());

    CudaQwenModel::Gradients gradients = resident.make_gradients(device);
    ASSERT_EQ(gradients.size(), resident.parameter_count());
    zetta::compute::ResidentTensor grad_upstream =
        zetta::compute::ResidentTensor::upload(device, upstream, {seq, d});
    zetta::compute::ResidentTensor grad_hidden;
    const auto status =
        resident.backward_layer(device, cache, grad_upstream, 0, seq, gradients, grad_hidden);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_TRUE(grad_hidden.valid());

    std::vector<float> analytic(elements, 0.0F);
    ASSERT_TRUE(grad_hidden.download(std::span<float>(analytic)).ok());

    const double step = 2e-3;
    for (std::size_t index = 0; index < elements; ++index) {
        std::vector<float> plus = hidden;
        std::vector<float> minus = hidden;
        plus[index] = static_cast<float>(plus[index] + step);
        minus[index] = static_cast<float>(minus[index] - step);
        const double numerical = (loss_for(plus) - loss_for(minus)) / (2.0 * step);
        EXPECT_NEAR(analytic[index], numerical, 5e-2) << "grad_hidden at " << index;
    }
}


// Rungs 6c + 7 + 8: a whole training step on-device, and the checkpoint it produces.
TEST(CudaQwenModel, TrainStepReducesLossAndUpdatesWeights) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const QwenConfig config = tiny_config();
    QwenModel host(config, /*seed=*/41);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    ASSERT_TRUE(resident.valid());

    auto gradients = resident.make_gradients(device);
    auto moment1 = resident.make_gradients(device);
    auto moment2 = resident.make_gradients(device);

    const std::vector<int> ids = {4, 12, 7, 21, 3, 18};
    const std::vector<int> targets(ids.begin() + 1, ids.end());

    // A weight before training, to prove the step actually moved something.
    std::vector<float> before(resident.parameter(1).element_count(), 0.0F);
    ASSERT_TRUE(resident.parameter(1).download(std::span<float>(before)).ok());

    float first_loss = 0.0F;
    float last_loss = 0.0F;
    for (int step = 1; step <= 40; ++step) {
        float loss = 0.0F;
        const auto status = resident.train_step(device, ids, targets, gradients, moment1, moment2,
                                                step, /*learning_rate=*/1e-2F, loss);
        ASSERT_TRUE(status.ok()) << "step " << step << ": " << status.message;
        ASSERT_TRUE(std::isfinite(loss)) << "step " << step << " produced a non-finite loss";
        if (step == 1) first_loss = loss;
        last_loss = loss;
    }
    // Overfitting one short sequence is the standard smoke test that forward, backward and the
    // optimizer agree: if any of the three is wrong, the loss does not fall.
    EXPECT_LT(last_loss, first_loss) << "training must reduce the loss on a fixed batch";

    std::vector<float> after(before.size(), 0.0F);
    ASSERT_TRUE(resident.parameter(1).download(std::span<float>(after)).ok());
    EXPECT_NE(before, after) << "AdamW must actually update the weights";
}

TEST(CudaQwenModel, TrainedWeightsRoundTripBackToTheHostModel) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    QwenModel host(tiny_config(), /*seed=*/43);
    CudaQwenModel resident = CudaQwenModel::from_host(device, host);
    auto gradients = resident.make_gradients(device);
    auto moment1 = resident.make_gradients(device);
    auto moment2 = resident.make_gradients(device);
    const std::vector<int> ids = {1, 5, 9, 2};
    const std::vector<int> targets(ids.begin() + 1, ids.end());
    float loss = 0.0F;
    ASSERT_TRUE(
        resident.train_step(device, ids, targets, gradients, moment1, moment2, 1, 1e-2F, loss).ok());

    // A checkpoint only its own backend can read is a trap: the whole point is training in one
    // place and serving in another. Copy back into the host model and check it took.
    ASSERT_TRUE(resident.download_into(host).ok());
    for (std::size_t index = 0; index < resident.parameter_count(); ++index) {
        std::vector<float> device_values(resident.parameter(index).element_count(), 0.0F);
        ASSERT_TRUE(resident.parameter(index).download(std::span<float>(device_values)).ok());
        EXPECT_EQ(host.parameters()[index].data(), device_values) << "parameter " << index;
    }
    // And the host model still runs — a corrupted round trip would surface here.
    const zetta::autograd::Tensor logits = host.forward(ids);
    for (const float value : logits.data()) ASSERT_TRUE(std::isfinite(value));
}


// Rung 9 of #923: the acceptance measurement, on the REAL checkpoint.
//
// Env-gated, never a CI dependency:
//   ZETTA_QWEN_DIR    the Qwen3-0.6B checkpoint
//   ZETTA_BENCH_STEPS steps to time (default 12)
//   ZETTA_BENCH_SEQ   sequence length (default 512)
//
// Reports per-step time and resident VRAM. The bar: MLX on a Mac does 1.45 s/step; the autograd
// path on this same A100 does 15.4 s/step. A rented GPU slower than a laptop is not worth using.
TEST(CudaQwenModel, AcceptanceBeatsTheMlxBaseline) {
    const char* model_dir = std::getenv("ZETTA_QWEN_DIR");
    if (model_dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to run the acceptance benchmark";
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    std::string error;
    std::optional<QwenModel> host = zetta::autograd::load_qwen_model(model_dir, &error);
    ASSERT_TRUE(host.has_value()) << error;

    CudaQwenModel resident = CudaQwenModel::from_host(device, *host);
    ASSERT_TRUE(resident.valid());
    const double resident_gb =
        static_cast<double>(resident.resident_bytes()) / (1024.0 * 1024.0 * 1024.0);
    std::cerr << "[bench] resident weights: " << resident_gb << " GB across "
              << resident.parameter_count() << " parameters\n";

    auto gradients = resident.make_gradients(device);
    auto moment1 = resident.make_gradients(device);
    auto moment2 = resident.make_gradients(device);

    const int steps = std::getenv("ZETTA_BENCH_STEPS")
                          ? std::atoi(std::getenv("ZETTA_BENCH_STEPS")) : 12;
    const int seq = std::getenv("ZETTA_BENCH_SEQ") ? std::atoi(std::getenv("ZETTA_BENCH_SEQ")) : 512;
    std::vector<int> ids(static_cast<std::size_t>(seq));
    for (int i = 0; i < seq; ++i) ids[static_cast<std::size_t>(i)] = (i * 7919) % 30000;
    const std::vector<int> targets(ids.begin() + 1, ids.end());

    // One untimed step first: the pool is empty on the first pass, so including it would measure
    // allocation rather than throughput.
    float loss = 0.0F;
    ASSERT_TRUE(
        resident.train_step(device, ids, targets, gradients, moment1, moment2, 1, 1e-5F, loss).ok());

    const auto started = std::chrono::steady_clock::now();
    for (int step = 2; step <= steps + 1; ++step) {
        const auto status = resident.train_step(device, ids, targets, gradients, moment1, moment2,
                                                step, /*learning_rate=*/1e-5F, loss);
        ASSERT_TRUE(status.ok()) << "step " << step << ": " << status.message;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double per_step = seconds / steps;

    std::cerr << "[bench] " << steps << " steps at seq " << seq << ": " << per_step
              << " s/step  (MLX on a Mac: 1.45; autograd on this box: 15.4)\n";
    std::cerr << "[bench] final loss " << loss << "\n";
    EXPECT_TRUE(std::isfinite(loss));
    EXPECT_LT(per_step, 1.45) << "a rented A100 must beat the laptop";
}

#endif  // ZETTA_WITH_CUDA

}  // namespace
