// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <vector>

#include "zetta/agent/linear_policy.hpp"
#include "zetta/compute/cpu_device.hpp"

#if defined(ZETTA_WITH_MLX)
#include "zetta/compute/mlx_device.hpp"
#endif

namespace {

using zetta::agent::LinearPolicy;
using zetta::compute::CpuDevice;

// W = [[1,2,3],[4,5,6]] (2 actions x 3 features); x = [1,0,1] -> logits [4, 10].
LinearPolicy make_policy() {
    return LinearPolicy(2, 3, {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F});
}

TEST(LinearPolicy, LogitsAreTheMatrixVectorProductOnTheDevice) {
    CpuDevice device;
    const std::vector<float> logits = make_policy().logits(device, {1.0F, 0.0F, 1.0F});
    ASSERT_EQ(logits.size(), 2u);
    EXPECT_FLOAT_EQ(logits[0], 4.0F);   // 1*1 + 2*0 + 3*1
    EXPECT_FLOAT_EQ(logits[1], 10.0F);  // 4*1 + 5*0 + 6*1
}

TEST(LinearPolicy, ProbabilitiesAreSoftmaxOfTheLogits) {
    CpuDevice device;
    const std::vector<double> probs =
        make_policy().probabilities(device, {1.0F, 0.0F, 1.0F});
    ASSERT_EQ(probs.size(), 2u);
    EXPECT_GT(probs[1], probs[0]);  // logit 10 > 4
    EXPECT_NEAR(probs[0] + probs[1], 1.0, 1e-12);
}

TEST(LinearPolicy, ShapeMismatchReturnsEmpty) {
    CpuDevice device;
    EXPECT_TRUE(make_policy().logits(device, {1.0F, 2.0F}).empty());  // wrong length
}

#if defined(ZETTA_WITH_MLX)
// The same forward pass on the GPU produces the same logits — the policy is
// backend-agnostic.
TEST(LinearPolicy, SameLogitsOnMlxAsOnCpu) {
    CpuDevice cpu;
    zetta::compute::MlxDevice mlx;
    const std::vector<float> features = {1.0F, 0.0F, 1.0F};
    const std::vector<float> on_cpu = make_policy().logits(cpu, features);
    const std::vector<float> on_gpu = make_policy().logits(mlx, features);
    ASSERT_EQ(on_cpu.size(), on_gpu.size());
    for (std::size_t i = 0; i < on_cpu.size(); ++i) {
        EXPECT_FLOAT_EQ(on_cpu[i], on_gpu[i]);
    }
}
#endif

}  // namespace
