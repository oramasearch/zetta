#include "zetta/agent/linear_policy.hpp"

#include <span>
#include <utility>

#include "zetta/agent/softmax_policy.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/device_buffer.hpp"

namespace zetta::agent {

LinearPolicy::LinearPolicy(std::size_t action_count, std::size_t feature_count,
                           std::vector<float> weights)
    : action_count_(action_count),
      feature_count_(feature_count),
      weights_(std::move(weights)) {}

std::vector<float> LinearPolicy::logits(
    compute::ComputeDevice& device, const std::vector<float>& features) const {
    if (features.size() != feature_count_ ||
        weights_.size() != action_count_ * feature_count_) {
        return {};
    }

    // W (action_count x feature_count) * x (feature_count x 1) = logits
    // (action_count x 1). All operands live on the device.
    compute::DeviceBuffer weight_buffer =
        device.upload<float>(std::span<const float>(weights_));
    compute::DeviceBuffer feature_buffer =
        device.upload<float>(std::span<const float>(features));
    compute::DeviceBuffer logit_buffer =
        device.allocate(compute::DataType::Float32, action_count_);
    if (weight_buffer.empty() || feature_buffer.empty() ||
        logit_buffer.empty()) {
        return {};
    }

    compute::GemmParams params;
    params.rows = action_count_;
    params.inner = feature_count_;
    params.cols = 1;
    params.alpha = 1.0;
    params.beta = 0.0;
    if (!device.gemm(params, weight_buffer, feature_buffer, logit_buffer).ok()) {
        return {};
    }

    std::vector<float> result(action_count_, 0.0F);
    if (!device.download<float>(logit_buffer, std::span<float>(result)).ok()) {
        return {};
    }
    return result;
}

std::vector<double> LinearPolicy::probabilities(
    compute::ComputeDevice& device, const std::vector<float>& features) const {
    const std::vector<float> raw = logits(device, features);
    if (raw.empty()) {
        return {};
    }
    // Reuse the reference policy's numerically-stable softmax over the logits.
    const SoftmaxPolicy distribution(std::vector<double>(raw.begin(), raw.end()));
    return distribution.probabilities();
}

}  // namespace zetta::agent
