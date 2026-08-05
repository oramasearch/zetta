#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "zetta/compute/data_type.hpp"

#if defined(ZETTA_WITH_CUDA)
#include "zetta/compute/cuda_device.hpp"
#include "zetta/compute/cuda_resident.hpp"
#endif

// Rung 1 of #923: a tensor that lives on the device across operations.
//
// This is the primitive the whole issue stands on. Today `device_gemm` uploads its operands and
// downloads its result on every call, which is why training holds 425 MiB of a 40 GB card and runs
// the GPU at ~11% (#928). "Resident" has to mean something testable, so this test defines it:
// upload once, do other device work in between, and find the SAME storage still holding the SAME
// bytes — never re-uploaded, never freed underneath us.
namespace {

#if defined(ZETTA_WITH_CUDA)

using zetta::compute::CudaDevice;
using zetta::compute::DataType;
using zetta::compute::ResidentTensor;

bool cuda_present(CudaDevice& device) {
    const auto info = device.info();
    return !(info.name.empty() || info.name == "cuda");
}

std::vector<float> ramp(std::size_t count) {
    std::vector<float> values(count);
    for (std::size_t index = 0; index < count; ++index) {
        values[index] = static_cast<float>(index) * 0.25F - 3.0F;
    }
    return values;
}

TEST(CudaResidentTensor, KeepsItsStorageAndContentsAcrossOtherDeviceWork) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const std::vector<float> weights = ramp(4096);
    ResidentTensor resident = ResidentTensor::upload(device, weights, {64, 64});
    ASSERT_TRUE(resident.valid());
    EXPECT_EQ(resident.element_count(), weights.size());
    EXPECT_EQ(resident.shape(), (std::vector<int>{64, 64}));

    const void* original_storage = resident.handle();
    ASSERT_NE(original_storage, nullptr);

    // Churn the allocator in between: this is what a training step does constantly, and it is
    // exactly the traffic that would expose a "resident" tensor that is quietly re-uploaded or
    // whose block gets recycled underneath it.
    for (int round = 0; round < 8; ++round) {
        auto scratch = device.allocate(DataType::Float32, 4096);
        ASSERT_FALSE(scratch.empty());
    }

    // Same storage, still ours.
    EXPECT_EQ(resident.handle(), original_storage)
        << "a resident tensor must not move or be re-uploaded";

    std::vector<float> read_back(weights.size(), 0.0F);
    ASSERT_TRUE(resident.download(std::span<float>(read_back)).ok());
    EXPECT_EQ(read_back, weights) << "resident contents changed under allocator churn";
}

TEST(CudaResidentTensor, MovesWithoutFreeingAndReportsItsBytes) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const std::vector<float> values = ramp(1024);
    ResidentTensor first = ResidentTensor::upload(device, values, {1024});
    const void* storage = first.handle();
    EXPECT_EQ(first.size_bytes(), values.size() * sizeof(float));

    // Move must transfer ownership without a free-and-realloc: the model holds these in a vector
    // that grows during load, so a move that re-uploaded would silently undo residency.
    ResidentTensor second = std::move(first);
    EXPECT_EQ(second.handle(), storage);
    EXPECT_FALSE(first.valid());

    std::vector<float> read_back(values.size(), 0.0F);
    ASSERT_TRUE(second.download(std::span<float>(read_back)).ok());
    EXPECT_EQ(read_back, values);
}

TEST(CudaResidentTensor, AnEmptyUploadIsInvalidRatherThanASilentZeroBuffer) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";
    // A zero-element weight means the checkpoint is malformed. Returning a valid-looking empty
    // tensor would push that failure into a kernel launch, where it reads as corrupt numbers.
    ResidentTensor empty = ResidentTensor::upload(device, std::vector<float>{}, {0});
    EXPECT_FALSE(empty.valid());
}

#endif  // ZETTA_WITH_CUDA

}  // namespace
