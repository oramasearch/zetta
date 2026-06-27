#include "zetta/ddf/join.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <utility>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"

namespace zetta::ddf {

using compute::ComputeDevice;
using compute::ComputeStatus;
using compute::DataType;

namespace {

// Download a column to host as its native type, then widen each value to double —
// the value-level join and the collective plane both work in float64.
template <class Element>
ComputeStatus download_widened(const ComputeDevice& device, const Column& column,
                               std::vector<double>& out) {
    std::vector<Element> native;
    const ComputeStatus status = column.to_host<Element>(device, native);
    if (!status.ok()) return status;
    out.assign(native.begin(), native.end());
    return ComputeStatus::success();
}

// Read `column` into `out` as float64, dispatching on its runtime dtype.
ComputeStatus column_to_double(const ComputeDevice& device, const Column& column,
                               std::vector<double>& out) {
    switch (column.dtype()) {
        case DataType::Float32:
            return download_widened<float>(device, column, out);
        case DataType::Float64:
            return column.to_host<double>(device, out);
        case DataType::Int32:
            return download_widened<std::int32_t>(device, column, out);
        case DataType::Int64:
            return download_widened<std::int64_t>(device, column, out);
        case DataType::UInt32:
            return download_widened<std::uint32_t>(device, column, out);
        case DataType::UInt64:
            return download_widened<std::uint64_t>(device, column, out);
        case DataType::UInt8:
            return download_widened<std::uint8_t>(device, column, out);
    }
    return ComputeStatus::invalid_argument(
        "to_host_relation: column '" + column.name() + "' has an unknown dtype");
}

// The output column names: the key, then left's value columns, then right's.
std::vector<std::string> joined_columns(const HostRelation& left,
                                        const HostRelation& right) {
    std::vector<std::string> columns;
    columns.reserve(1 + left.width() + right.width());
    columns.push_back(left.key_name);
    for (const std::string& name : left.value_columns) columns.push_back(name);
    for (const std::string& name : right.value_columns) columns.push_back(name);
    return columns;
}

// Append one joined output row (key + left payload + right payload) to `out`.
void emit_row(JoinedRows& out, const HostRelation& left, std::size_t left_row,
              const HostRelation& right, std::size_t right_row) {
    out.data.push_back(left.keys[left_row]);
    const std::size_t left_width = left.width();
    for (std::size_t column = 0; column < left_width; ++column) {
        out.data.push_back(left.values[left_row * left_width + column]);
    }
    const std::size_t right_width = right.width();
    for (std::size_t column = 0; column < right_width; ++column) {
        out.data.push_back(right.values[right_row * right_width + column]);
    }
}

}  // namespace

// --- Materialize a frame into a host relation --------------------------------

HostRelationResult to_host_relation(const DataFrame& frame,
                                    std::string_view key_column) {
    HostRelationResult result;
    const Column* key = frame.column(key_column);
    if (key == nullptr) {
        result.status = ComputeStatus::invalid_argument(
            "to_host_relation: no key column named '" + std::string{key_column} +
            "'");
        return result;
    }
    ComputeDevice* device = frame.device();
    if (device == nullptr) {
        result.status = ComputeStatus::invalid_argument(
            "to_host_relation: the frame has no device to read from");
        return result;
    }

    HostRelation relation;
    relation.key_name = std::string{key_column};

    const ComputeStatus key_status = column_to_double(*device, *key, relation.keys);
    if (!key_status.ok()) {
        result.status = key_status;
        return result;
    }
    const std::size_t row_count = relation.keys.size();

    // Every other column, in frame order, becomes a value column.
    std::vector<std::vector<double>> value_data;
    for (const Column& column : frame.columns()) {
        if (column.name() == key_column) continue;
        relation.value_columns.push_back(column.name());
        std::vector<double> values;
        const ComputeStatus status = column_to_double(*device, column, values);
        if (!status.ok()) {
            result.status = status;
            return result;
        }
        value_data.push_back(std::move(values));
    }

    // Transpose the per-column vectors into a row-major payload buffer.
    const std::size_t width = relation.value_columns.size();
    relation.values.resize(row_count * width);
    for (std::size_t column = 0; column < width; ++column) {
        const std::vector<double>& source = value_data[column];
        for (std::size_t row = 0; row < row_count; ++row) {
            relation.values[row * width + column] = source[row];
        }
    }

    result.relation = std::move(relation);
    result.status = ComputeStatus::success();
    return result;
}

// --- Local joins -------------------------------------------------------------

JoinedRows hash_join(const HostRelation& left, const HostRelation& right) {
    JoinedRows out;
    out.columns = joined_columns(left, right);

    // Build side: key -> the right rows carrying it (duplicates kept, so an inner
    // join emits the full cartesian product of a key's matching rows).
    std::unordered_map<double, std::vector<std::size_t>> index;
    index.reserve(right.row_count());
    for (std::size_t row = 0; row < right.row_count(); ++row) {
        const double key = right.keys[row];
        if (std::isnan(key)) continue;  // NaN never equi-matches
        index[key].push_back(row);
    }

    // Probe side: each left row against the build index.
    for (std::size_t row = 0; row < left.row_count(); ++row) {
        const double key = left.keys[row];
        if (std::isnan(key)) continue;
        const auto found = index.find(key);
        if (found == index.end()) continue;
        for (const std::size_t right_row : found->second) {
            emit_row(out, left, row, right, right_row);
        }
    }
    return out;
}

JoinedRows sort_merge_join(const HostRelation& left, const HostRelation& right) {
    JoinedRows out;
    out.columns = joined_columns(left, right);

    // Row indices sorted by key, NaN keys dropped (they cannot equi-match, and
    // dropping them keeps the merge total-ordered so it always makes progress).
    auto sorted_by_key = [](const HostRelation& relation) {
        std::vector<std::size_t> order;
        order.reserve(relation.row_count());
        for (std::size_t row = 0; row < relation.row_count(); ++row) {
            if (!std::isnan(relation.keys[row])) order.push_back(row);
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return relation.keys[a] < relation.keys[b];
        });
        return order;
    };
    const std::vector<std::size_t> left_order = sorted_by_key(left);
    const std::vector<std::size_t> right_order = sorted_by_key(right);

    std::size_t i = 0;
    std::size_t j = 0;
    const std::size_t left_n = left_order.size();
    const std::size_t right_n = right_order.size();
    while (i < left_n && j < right_n) {
        const double left_key = left.keys[left_order[i]];
        const double right_key = right.keys[right_order[j]];
        if (left_key < right_key) {
            ++i;
        } else if (right_key < left_key) {
            ++j;
        } else {
            // Equal-key groups on both sides -> their cartesian product.
            const std::size_t left_start = i;
            while (i < left_n && left.keys[left_order[i]] == left_key) ++i;
            const std::size_t right_start = j;
            while (j < right_n && right.keys[right_order[j]] == right_key) ++j;
            for (std::size_t a = left_start; a < i; ++a) {
                for (std::size_t b = right_start; b < j; ++b) {
                    emit_row(out, left, left_order[a], right, right_order[b]);
                }
            }
        }
    }
    return out;
}

JoinedRows local_join(const HostRelation& left, const HostRelation& right,
                      JoinAlgorithm algorithm) {
    return algorithm == JoinAlgorithm::SortMerge ? sort_merge_join(left, right)
                                                 : hash_join(left, right);
}

void sort_rows(JoinedRows& rows) {
    const std::size_t width = rows.width();
    if (width == 0) return;
    const std::size_t row_count = rows.row_count();
    std::vector<std::size_t> order(row_count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        for (std::size_t column = 0; column < width; ++column) {
            const double x = rows.data[a * width + column];
            const double y = rows.data[b * width + column];
            if (x < y) return true;
            if (y < x) return false;
        }
        return false;
    });
    std::vector<double> sorted(rows.data.size());
    for (std::size_t row = 0; row < row_count; ++row) {
        for (std::size_t column = 0; column < width; ++column) {
            sorted[row * width + column] = rows.data[order[row] * width + column];
        }
    }
    rows.data = std::move(sorted);
}

// --- Repartition primitives --------------------------------------------------

std::size_t partition_index(double key, std::size_t participant_count) {
    if (participant_count <= 1) return 0;
    return std::hash<double>{}(key) % participant_count;
}

HostRelation filter_partition(const HostRelation& relation, std::size_t index,
                              std::size_t participant_count) {
    HostRelation out;
    out.key_name = relation.key_name;
    out.value_columns = relation.value_columns;
    const std::size_t width = relation.width();
    for (std::size_t row = 0; row < relation.row_count(); ++row) {
        if (partition_index(relation.keys[row], participant_count) != index) {
            continue;
        }
        out.keys.push_back(relation.keys[row]);
        for (std::size_t column = 0; column < width; ++column) {
            out.values.push_back(relation.values[row * width + column]);
        }
    }
    return out;
}

std::vector<double> serialize_relation(const HostRelation& relation) {
    const std::size_t width = relation.width();
    std::vector<double> blob;
    blob.reserve(relation.row_count() * (1 + width));
    for (std::size_t row = 0; row < relation.row_count(); ++row) {
        blob.push_back(relation.keys[row]);
        for (std::size_t column = 0; column < width; ++column) {
            blob.push_back(relation.values[row * width + column]);
        }
    }
    return blob;
}

std::optional<HostRelation> deserialize_relation(
    const std::vector<double>& blob, std::string key_name,
    std::vector<std::string> value_columns) {
    const std::size_t width = 1 + value_columns.size();  // key + payload
    if (blob.size() % width != 0) {
        return std::nullopt;  // not a whole number of rows -> malformed/truncated
    }
    const std::size_t row_count = blob.size() / width;
    HostRelation relation;
    relation.key_name = std::move(key_name);
    relation.value_columns = std::move(value_columns);
    relation.keys.reserve(row_count);
    relation.values.reserve(row_count * (width - 1));
    for (std::size_t row = 0; row < row_count; ++row) {
        const std::size_t base = row * width;
        relation.keys.push_back(blob[base]);
        for (std::size_t column = 1; column < width; ++column) {
            relation.values.push_back(blob[base + column]);
        }
    }
    return relation;
}

// --- Distributed join --------------------------------------------------------

namespace {

// All-to-all one relation across the cluster via the only collectives available:
// every node gathers its serialized rows to the root, the root concatenates them,
// and a broadcast hands the full relation back to everyone. Returns the full
// relation (re-labeled with the local, SPMD-identical schema) on every node, or a
// failing status on a collective failure or a malformed broadcast.
std::optional<HostRelation> repartition(compute::JobRound& round,
                                        const HostRelation& shard,
                                        std::string_view tag,
                                        ComputeStatus& status) {
    const std::vector<double> blob = serialize_relation(shard);

    const auto gathered = round.gather(std::string{tag} + ".gather", blob);
    if (!gathered) {
        status = ComputeStatus::backend_error(
            "distributed_join: gather failed for '" + std::string{tag} +
            "' (unreachable peer or timeout)");
        return std::nullopt;
    }
    // The root concatenates every node's rows in member order; non-root nodes send
    // an empty value (broadcast distributes only the root's).
    std::vector<double> combined;
    if (round.is_root()) {
        for (const std::vector<double>& part : *gathered) {
            combined.insert(combined.end(), part.begin(), part.end());
        }
    }
    const auto full = round.broadcast(std::string{tag} + ".bcast", combined);
    if (!full) {
        status = ComputeStatus::backend_error(
            "distributed_join: broadcast failed for '" + std::string{tag} +
            "' (unreachable peer or timeout)");
        return std::nullopt;
    }
    std::optional<HostRelation> relation =
        deserialize_relation(*full, shard.key_name, shard.value_columns);
    if (!relation) {
        status = ComputeStatus::invalid_argument(
            "distributed_join: malformed shuffled relation for '" +
            std::string{tag} + "'");
        return std::nullopt;
    }
    return relation;
}

}  // namespace

DistributedJoinResult distributed_join(compute::JobRound& round,
                                       const DataFrame& left,
                                       std::string_view left_key,
                                       const DataFrame& right,
                                       std::string_view right_key,
                                       JoinAlgorithm algorithm,
                                       std::string_view tag) {
    DistributedJoinResult result;

    HostRelationResult left_shard = to_host_relation(left, left_key);
    if (!left_shard.ok()) {
        result.status = left_shard.status;
        return result;
    }
    HostRelationResult right_shard = to_host_relation(right, right_key);
    if (!right_shard.ok()) {
        result.status = right_shard.status;
        return result;
    }

    // This node's partition index among the (sorted) participants.
    const std::vector<std::string> participants = round.participants();
    const std::size_t participant_count = participants.size();
    const auto self = std::find(participants.begin(), participants.end(),
                                round.node_id());
    if (self == participants.end()) {
        result.status = ComputeStatus::backend_error(
            "distributed_join: this node is not among the collective participants");
        return result;
    }
    const std::size_t my_index =
        static_cast<std::size_t>(std::distance(participants.begin(), self));

    // Repartition both relations by key, then keep only this node's partition.
    const std::string base_tag{tag};
    std::optional<HostRelation> full_left =
        repartition(round, left_shard.relation, base_tag + ".L", result.status);
    if (!full_left) return result;
    std::optional<HostRelation> full_right =
        repartition(round, right_shard.relation, base_tag + ".R", result.status);
    if (!full_right) return result;

    const HostRelation left_partition =
        filter_partition(*full_left, my_index, participant_count);
    const HostRelation right_partition =
        filter_partition(*full_right, my_index, participant_count);

    result.rows = local_join(left_partition, right_partition, algorithm);
    result.status = ComputeStatus::success();
    return result;
}

}  // namespace zetta::ddf
