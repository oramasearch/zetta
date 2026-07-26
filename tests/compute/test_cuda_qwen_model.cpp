#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#if defined(ZETTA_WITH_CUDA)
#include "zetta/autograd/qwen.hpp"
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

#endif  // ZETTA_WITH_CUDA

}  // namespace
