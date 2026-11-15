// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <vector>

#include "zetta/compute/compute_device.hpp"

// A linear policy whose action logits are a matrix-vector product computed ON a
// ComputeDevice: logits = W * x, where W is (action_count x feature_count),
// row-major, and x is the input feature vector (#555; docs/agent/README.md).
// softmax(logits) gives the action distribution.
//
// This is the bridge from the abstract reference policy (SoftmaxPolicy, whose
// logits are free parameters) to REAL tensor compute: the same forward pass runs
// on CpuDevice (the always-present numerical reference) and on MlxDevice (the GPU),
// because the device is injected. A real transformer later is just more layers
// behind this same interface — the RLVR training loop above it does not change.
namespace zetta::agent {

class LinearPolicy {
public:
    // `weights` is the row-major action_count x feature_count matrix W.
    LinearPolicy(std::size_t action_count, std::size_t feature_count,
                 std::vector<float> weights);

    std::size_t action_count() const noexcept { return action_count_; }
    std::size_t feature_count() const noexcept { return feature_count_; }

    std::vector<float>& weights() noexcept { return weights_; }  // for updates
    const std::vector<float>& weights() const noexcept { return weights_; }

    // logits = W * features, computed on `device` via gemm. Returns an empty vector
    // on a shape mismatch or a device error (a non-throwing boundary).
    std::vector<float> logits(compute::ComputeDevice& device,
                              const std::vector<float>& features) const;

    // softmax(logits(...)) — the action probabilities (empty on error).
    std::vector<double> probabilities(compute::ComputeDevice& device,
                                      const std::vector<float>& features) const;

private:
    std::size_t action_count_;
    std::size_t feature_count_;
    std::vector<float> weights_;  // action_count x feature_count, row-major
};

}  // namespace zetta::agent
