#include <gtest/gtest.h>

// The autograd model is device-agnostic: the same graph runs on CpuDevice or
// MlxDevice (or CudaDevice on a CUDA host). This guarded test compares CPU vs the
// MLX GPU; it compiles only under ZETTA_WITH_MLX.
#if defined(ZETTA_WITH_MLX)

#include <cstddef>
#include <vector>

#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/autograd/transformer.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/mlx_device.hpp"

namespace {

using zetta::autograd::QwenConfig;
using zetta::autograd::QwenModel;
using zetta::autograd::set_compute_device;
using zetta::autograd::Tensor;
using zetta::autograd::Transformer;
using zetta::autograd::TransformerConfig;
using zetta::compute::CpuDevice;
using zetta::compute::MlxDevice;

TransformerConfig config() {
    TransformerConfig c;
    c.vocab_size = 12;
    c.d_model = 16;
    c.n_layers = 2;
    c.d_ff = 32;
    c.max_seq = 8;
    return c;
}

// The same model's forward pass produces matching logits on the CPU and the MLX GPU
// — one model, any device, so a distributed model can span CPU + GPU + MLX nodes.
TEST(AutogradDeviceAgnostic, TransformerForwardCpuEqualsMlx) {
    const Transformer model(config(), 42);
    const std::vector<int> ids = {1, 2, 3, 4, 5};

    CpuDevice cpu;
    MlxDevice mlx;

    set_compute_device(&cpu);
    const std::vector<float> on_cpu = model.forward(ids).data();
    set_compute_device(&mlx);
    const std::vector<float> on_mlx = model.forward(ids).data();
    set_compute_device(nullptr);

    ASSERT_EQ(on_cpu.size(), on_mlx.size());
    for (std::size_t i = 0; i < on_cpu.size(); ++i) {
        EXPECT_NEAR(on_cpu[i], on_mlx[i], 2e-3F) << "logit " << i;
    }
}

QwenConfig qwen_config() {
    QwenConfig c;
    c.vocab_size = 24;
    c.d_model = 16;
    c.n_layers = 2;
    c.n_heads = 4;
    c.n_kv_heads = 2;
    c.head_dim = 4;
    c.d_ff = 32;
    return c;
}

// The Qwen3-parity model runs on the MLX GPU too: its projections go through
// matmul_transposed_b, whose ComputeDevice path issues an MLX gemm (transpose_b).
// Same logits on CPU and GPU — the real model is device-agnostic.
TEST(AutogradDeviceAgnostic, QwenModelForwardCpuEqualsMlx) {
    const QwenModel model(qwen_config(), 7);
    const std::vector<int> ids = {1, 5, 2, 7, 3};

    CpuDevice cpu;
    MlxDevice mlx;

    set_compute_device(&cpu);
    const std::vector<float> on_cpu = model.forward(ids).data();
    set_compute_device(&mlx);
    const std::vector<float> on_mlx = model.forward(ids).data();
    set_compute_device(nullptr);

    ASSERT_EQ(on_cpu.size(), on_mlx.size());
    for (std::size_t i = 0; i < on_cpu.size(); ++i) {
        EXPECT_NEAR(on_cpu[i], on_mlx[i], 3e-3F) << "logit " << i;
    }
}

}  // namespace

#endif  // ZETTA_WITH_MLX
