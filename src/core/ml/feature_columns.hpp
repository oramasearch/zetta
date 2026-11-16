// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/data_frame.hpp"

// Shared device-download helper for the ml job bodies (train + score + explain). A
// feature frame's columns live on its `ComputeDevice` (a CPU or CUDA frame, e.g. the
// one `ddf::FeatureTable::to_data_frame` materializes); reading them back as host
// doubles is the "host-side reference over device-backed data" seam the GBDT trainer
// (gbdt_train_job.cpp), the scorer (gbdt_score_job.cpp), and the TreeSHAP explainer
// share. Internal to zetta-core (impl detail, like ddf/data_frame_internal.hpp) — not
// a public API.
namespace zetta::ml::detail {

// Read a frame column into a host double vector, widening whatever numeric dtype it
// carries (Float64 features + an Int64/Float64 label both occur). Non-throwing —
// returns the device's status on a read-back error, or InvalidArgument for a missing
// / non-numeric column.
inline compute::ComputeStatus read_column_as_double(const ddf::DataFrame& frame,
                                                    const std::string& name,
                                                    std::vector<double>& out) {
    using compute::ComputeStatus;
    using compute::DataType;
    const ddf::Column* column = frame.column(name);
    if (column == nullptr) {
        return ComputeStatus::invalid_argument("ml: no column named '" + name + "'");
    }
    switch (column->dtype()) {
        case DataType::Float64:
            return frame.to_host<double>(name, out);
        case DataType::Float32: {
            std::vector<float> typed;
            const ComputeStatus status = frame.to_host<float>(name, typed);
            if (!status.ok()) return status;
            out.assign(typed.begin(), typed.end());
            return ComputeStatus::success();
        }
        case DataType::Int64: {
            std::vector<std::int64_t> typed;
            const ComputeStatus status = frame.to_host<std::int64_t>(name, typed);
            if (!status.ok()) return status;
            out.assign(typed.begin(), typed.end());
            return ComputeStatus::success();
        }
        case DataType::Int32: {
            std::vector<std::int32_t> typed;
            const ComputeStatus status = frame.to_host<std::int32_t>(name, typed);
            if (!status.ok()) return status;
            out.assign(typed.begin(), typed.end());
            return ComputeStatus::success();
        }
        case DataType::UInt64: {
            std::vector<std::uint64_t> typed;
            const ComputeStatus status = frame.to_host<std::uint64_t>(name, typed);
            if (!status.ok()) return status;
            out.assign(typed.begin(), typed.end());
            return ComputeStatus::success();
        }
        case DataType::UInt32: {
            std::vector<std::uint32_t> typed;
            const ComputeStatus status = frame.to_host<std::uint32_t>(name, typed);
            if (!status.ok()) return status;
            out.assign(typed.begin(), typed.end());
            return ComputeStatus::success();
        }
        case DataType::UInt8: {
            std::vector<std::uint8_t> typed;
            const ComputeStatus status = frame.to_host<std::uint8_t>(name, typed);
            if (!status.ok()) return status;
            out.assign(typed.begin(), typed.end());
            return ComputeStatus::success();
        }
    }
    return ComputeStatus::invalid_argument("ml: unsupported column dtype for '" +
                                           name + "'");
}

}  // namespace zetta::ml::detail
