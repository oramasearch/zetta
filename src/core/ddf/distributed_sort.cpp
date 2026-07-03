#include "zetta/ddf/distributed_sort.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/ddf/data_frame_ops.hpp"
#include "data_frame_internal.hpp"

namespace zetta::ddf {

using compute::ComputeStatus;
using compute::DataType;
using compute::SortOrder;
using internal::HostFrame;

namespace {

// The RPC method an all-to-all shuffle partition is shipped under.
constexpr std::string_view kShuffleMethod = "ddf.shuffle";

// The partition-message JSON Schema (draft-07) — the code-owned source of
// truth, exposed as ddf_partition_schema(); schemas/ddf_partition.schema.json
// is generated from it and drift-checked by tests/test_schemas.cpp (#646).
// Enforced at runtime (the same pattern as the collective message — see
// collectives.cpp). A closed object: `tag` / `rows` / `columns` required; each
// column is name + dtype (one of the seven DataType names) + a `values`
// number array.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.ddf.PartitionMessage",
  "description": "A DDF DataFrame partition shipped over the signal plane during a distributed sort's all-to-all shuffle (method ddf.shuffle), carried as the opaque payload of a signal. `tag` names the shuffle round; `from` is the sender's node id; `rows` is the partition's row count; `columns` carries the partition column-major (one entry per frame column, each with its name, dtype, and `rows` values). Validated at the boundary, exactly like the collective message.",
  "type": "object",
  "properties": {
    "tag":  { "type": "string" },
    "from": { "type": "string" },
    "rows": { "type": "integer", "minimum": 0 },
    "columns": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "name":  { "type": "string", "minLength": 1 },
          "dtype": {
            "type": "string",
            "enum": ["float32", "float64", "int32", "int64", "uint32", "uint64", "uint8"]
          },
          "values": { "type": "array", "items": { "type": "number" } }
        },
        "required": ["name", "dtype", "values"],
        "additionalProperties": false
      }
    }
  },
  "required": ["tag", "rows", "columns"],
  "additionalProperties": false
}
)JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure without letting it cross as an exception
// (matching collectives.cpp / signal.cpp).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

void set_error(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

std::optional<DataType> data_type_from_string(std::string_view text) {
    constexpr DataType kinds[] = {DataType::Float32, DataType::Float64,
                                  DataType::Int32,   DataType::Int64,
                                  DataType::UInt32,  DataType::UInt64,
                                  DataType::UInt8};
    for (const DataType kind : kinds) {
        if (compute::to_string(kind) == text) return kind;
    }
    return std::nullopt;
}

// Write `count` doubles into a typed byte blob (the inverse of column_doubles):
// the shuffle ships values as doubles, exact for every dtype whose range fits a
// double's 53-bit mantissa (so 64-bit integers beyond 2^53 are the documented
// limit, well past churn id / count ranges).
template <class Element>
std::vector<std::byte> doubles_to_bytes(const std::vector<double>& values) {
    std::vector<std::byte> bytes(values.size() * sizeof(Element));
    Element* out = reinterpret_cast<Element*>(bytes.data());
    for (std::size_t index = 0; index < values.size(); ++index) {
        out[index] = static_cast<Element>(values[index]);
    }
    return bytes;
}

std::vector<std::byte> column_bytes_from_doubles(DataType dtype,
                                                 const std::vector<double>& values) {
    switch (dtype) {
        case DataType::Float32: return doubles_to_bytes<float>(values);
        case DataType::Float64: return doubles_to_bytes<double>(values);
        case DataType::Int32:   return doubles_to_bytes<std::int32_t>(values);
        case DataType::Int64:   return doubles_to_bytes<std::int64_t>(values);
        case DataType::UInt32:  return doubles_to_bytes<std::uint32_t>(values);
        case DataType::UInt64:  return doubles_to_bytes<std::uint64_t>(values);
        case DataType::UInt8:   return doubles_to_bytes<std::uint8_t>(values);
    }
    return {};
}

// HostFrame -> PartitionMessage (every column read out as doubles).
PartitionMessage to_message(const std::string& tag, const std::string& from,
                            const HostFrame& host) {
    PartitionMessage message;
    message.tag = tag;
    message.from = from;
    message.rows = host.rows;
    message.columns.reserve(host.column_count());
    for (std::size_t index = 0; index < host.column_count(); ++index) {
        PartitionMessage::ColumnData column;
        column.name = host.names[index];
        column.dtype = host.dtypes[index];
        column.values =
            internal::column_doubles(host, host.names[index]).value_or(std::vector<double>{});
        message.columns.push_back(std::move(column));
    }
    return message;
}

// PartitionMessage -> HostFrame (each column's doubles written back to its dtype).
HostFrame from_message(const PartitionMessage& message) {
    HostFrame host;
    host.rows = message.rows;
    host.names.reserve(message.columns.size());
    host.dtypes.reserve(message.columns.size());
    host.data.reserve(message.columns.size());
    for (const PartitionMessage::ColumnData& column : message.columns) {
        host.names.push_back(column.name);
        host.dtypes.push_back(column.dtype);
        host.data.push_back(column_bytes_from_doubles(column.dtype, column.values));
    }
    return host;
}

// --- sample-sort host helpers -----------------------------------------------

// Evenly-spaced interior samples of `sorted_keys` (already in sort order). Picks up
// to `count` quantile values; none from an empty key vector.
std::vector<double> pick_samples(const std::vector<double>& sorted_keys,
                                 std::size_t count) {
    std::vector<double> samples;
    if (sorted_keys.empty() || count == 0) return samples;
    for (std::size_t index = 1; index <= count; ++index) {
        const std::size_t position = index * sorted_keys.size() / (count + 1);
        samples.push_back(sorted_keys[std::min(position, sorted_keys.size() - 1)]);
    }
    return samples;
}

// Choose P-1 splitters from the gathered samples: sort them in the order's
// direction, then take quantile values. Always returns exactly partition_count-1
// values (sorted in the order's direction), padding with 0.0 when there were no
// samples (every key then lands in partition 0 — which is fine, the data is empty).
std::vector<double> choose_splitters(std::vector<double> samples,
                                     std::size_t partition_count, SortOrder order) {
    if (partition_count <= 1) return {};
    if (order == SortOrder::Ascending) {
        std::sort(samples.begin(), samples.end());
    } else {
        std::sort(samples.begin(), samples.end(), std::greater<double>{});
    }
    std::vector<double> splitters;
    splitters.reserve(partition_count - 1);
    for (std::size_t k = 1; k < partition_count; ++k) {
        if (samples.empty()) {
            splitters.push_back(0.0);
            continue;
        }
        const std::size_t position = k * samples.size() / partition_count;
        splitters.push_back(samples[std::min(position, samples.size() - 1)]);
    }
    return splitters;
}

// The partition a key falls in, given splitters sorted in the order's direction.
// Ascending: count of splitters <= v. Descending: count of splitters >= v. Both
// monotonic in the sorted-key sequence, so partitions are contiguous row ranges.
std::size_t partition_of(double value, const std::vector<double>& splitters,
                         SortOrder order) {
    if (order == SortOrder::Ascending) {
        return static_cast<std::size_t>(
            std::upper_bound(splitters.begin(), splitters.end(), value) -
            splitters.begin());
    }
    return static_cast<std::size_t>(
        std::upper_bound(splitters.begin(), splitters.end(), value,
                         std::greater<double>{}) -
        splitters.begin());
}

// Split a locally-sorted host frame into `partition_count` buckets by the
// splitters. Since the frame is sorted and the partition function is monotonic,
// each bucket is a contiguous row range.
std::vector<HostFrame> partition_by_splitters(const HostFrame& sorted,
                                              const std::vector<double>& sorted_keys,
                                              const std::vector<double>& splitters,
                                              SortOrder order,
                                              std::size_t partition_count) {
    std::vector<std::size_t> bounds(partition_count + 1, sorted.rows);
    bounds[0] = 0;
    std::size_t next_partition = 1;
    for (std::size_t row = 0; row < sorted.rows && next_partition < partition_count;
         ++row) {
        const std::size_t target = partition_of(sorted_keys[row], splitters, order);
        while (next_partition <= target && next_partition < partition_count) {
            bounds[next_partition] = row;
            ++next_partition;
        }
    }
    std::vector<HostFrame> buckets;
    buckets.reserve(partition_count);
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        buckets.push_back(
            internal::slice_rows(sorted, bounds[partition], bounds[partition + 1]));
    }
    return buckets;
}

}  // namespace

// --- PartitionMessage JSON boundary -----------------------------------------

// The code-owned schema (the anonymous-namespace schema_document() above),
// exposed for generation + the drift check (#646).
const nlohmann::json& ddf_partition_schema() { return schema_document(); }

std::string encode_partition_message(const PartitionMessage& message) {
    nlohmann::json document{{"tag", message.tag}, {"rows", message.rows}};
    if (!message.from.empty()) document["from"] = message.from;
    nlohmann::json columns = nlohmann::json::array();
    for (const PartitionMessage::ColumnData& column : message.columns) {
        columns.push_back({{"name", column.name},
                           {"dtype", compute::to_string(column.dtype)},
                           {"values", column.values}});
    }
    document["columns"] = std::move(columns);
    return document.dump();
}

std::optional<PartitionMessage> decode_partition_message(std::string_view payload,
                                                         std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "partition message is not valid JSON");
        return std::nullopt;
    }
    collecting_error_handler handler;
    schema_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    PartitionMessage message;
    message.tag = document.value("tag", std::string{});
    message.from = document.value("from", std::string{});
    message.rows = document.at("rows").get<std::size_t>();
    for (const nlohmann::json& entry : document.at("columns")) {
        PartitionMessage::ColumnData column;
        column.name = entry.at("name").get<std::string>();
        const std::optional<DataType> dtype =
            data_type_from_string(entry.at("dtype").get<std::string>());
        if (!dtype) {  // unreachable past the schema enum, kept for defense in depth
            set_error(error, "/columns: unknown dtype '" +
                                 entry.at("dtype").get<std::string>() + "'");
            return std::nullopt;
        }
        column.dtype = *dtype;
        column.values = entry.at("values").get<std::vector<double>>();
        // Cross-field rule the schema can't express: every column has `rows` values.
        if (column.values.size() != message.rows) {
            set_error(error, "/columns/" + column.name + "/values: expected " +
                                 std::to_string(message.rows) + " values");
            return std::nullopt;
        }
        message.columns.push_back(std::move(column));
    }
    return message;
}

// --- DistributedSorter -------------------------------------------------------

DistributedSorter::DistributedSorter(zetta::cluster::Cluster& cluster, compute::ComputeDevice& device,
                                     SignalHandler* delegate)
    : cluster_(cluster),
      device_(device),
      delegate_(delegate),
      collectives_(cluster, delegate) {
    // Become the chain head: shuffle handled here, everything else forwarded to the
    // collectives (constructed above with `delegate` as its own fallback).
    cluster_.set_signal_handler(this);
}

DistributedSorter::~DistributedSorter() {
    // Restore the collectives as the cluster handler; its own destructor (running
    // next, as it is the last-declared member) then restores `delegate_`.
    cluster_.set_signal_handler(&collectives_);
}

zetta::cluster::SignalResponse DistributedSorter::on_request(const zetta::cluster::Signal& request) {
    if (request.method == kShuffleMethod) {
        std::string error;
        const std::optional<PartitionMessage> message =
            decode_partition_message(request.payload, &error);
        if (!message) {
            return zetta::cluster::SignalResponse{.ok = false, .error = "ddf.shuffle: " + error};
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            inbox_[message->tag].push_back(*message);
        }
        return zetta::cluster::SignalResponse{.ok = true};
    }
    return collectives_.on_request(request);
}

void DistributedSorter::on_event(const zetta::cluster::Signal& event) {
    collectives_.on_event(event);
}

void DistributedSorter::on_task(const zetta::cluster::Signal& task) {
    collectives_.on_task(task);
}

DistributedSorter::Result DistributedSorter::sort(const DataFrame& local_shard,
                                                  std::string_view key_column,
                                                  SortOrder order,
                                                  const std::string& round_tag,
                                                  std::chrono::milliseconds timeout) {
    const auto fail = [](std::string reason) -> Result {
        return {DataFrame{}, ComputeStatus::backend_error(std::move(reason))};
    };

    if (local_shard.device() == nullptr) {
        return {DataFrame{}, ComputeStatus::invalid_argument(
                                 "distributed sort: an empty shard has no device")};
    }
    const std::size_t chunk_rows = local_shard.chunk_rows() == 0
                                       ? kDefaultChunkRows
                                       : local_shard.chunk_rows();

    // 1. Local sort the shard (also validates the key column exists).
    HostFrame host;
    if (const ComputeStatus status = internal::download_frame(local_shard, host);
        !status.ok()) {
        return {DataFrame{}, status};
    }
    HostFrame sorted;
    if (const ComputeStatus status =
            internal::sort_host(device_, host, key_column, order, sorted);
        !status.ok()) {
        return {DataFrame{}, status};
    }

    const std::vector<std::string> members = collectives_.participants();
    const std::size_t partition_count = members.size();
    const std::string self = cluster_.node_id();

    // Single-node cluster: the locally-sorted shard is the whole answer.
    if (partition_count <= 1) {
        DataFrameResult result = internal::upload_frame(device_, sorted, chunk_rows);
        return {std::move(result.frame), result.status};
    }

    const auto rank_it = std::find(members.begin(), members.end(), self);
    if (rank_it == members.end()) {
        return fail("distributed sort: this node is not a participant");
    }
    const std::size_t rank = static_cast<std::size_t>(rank_it - members.begin());

    // 2. Sample local keys, gather to root, choose splitters, broadcast.
    const std::vector<double> sorted_keys =
        internal::column_doubles(sorted, key_column).value_or(std::vector<double>{});
    const std::vector<double> samples =
        pick_samples(sorted_keys, partition_count - 1);

    const std::optional<std::vector<std::vector<double>>> gathered =
        collectives_.gather(round_tag + ".sample", samples, timeout);
    if (!gathered) return fail("distributed sort: sample gather failed");

    std::vector<double> splitters;
    if (self == members.front()) {  // the root computes the splitters
        std::vector<double> all_samples;
        for (const std::vector<double>& part : *gathered) {
            all_samples.insert(all_samples.end(), part.begin(), part.end());
        }
        splitters = choose_splitters(std::move(all_samples), partition_count, order);
    }
    const std::optional<std::vector<double>> broadcast_splitters =
        collectives_.broadcast(round_tag + ".split", splitters, timeout);
    if (!broadcast_splitters) return fail("distributed sort: splitter broadcast failed");
    splitters = *broadcast_splitters;

    // 3. Partition the local sorted shard into one bucket per participant.
    std::vector<HostFrame> buckets = partition_by_splitters(
        sorted, sorted_keys, splitters, order, partition_count);

    // 4. Shuffle: ship bucket b to participant b (keep our own), then fence
    //    delivery with a barrier so every inbound bucket has arrived.
    const std::string shuffle_tag = round_tag + ".shuffle";
    for (std::size_t bucket = 0; bucket < partition_count; ++bucket) {
        if (bucket == rank) continue;
        const PartitionMessage message =
            to_message(shuffle_tag, self, buckets[bucket]);
        const zetta::cluster::SignalResponse response = cluster_.request(
            members[bucket], std::string{kShuffleMethod},
            encode_partition_message(message), timeout);
        if (!response.ok) {
            return fail("distributed sort: shuffle to " + members[bucket] +
                        " failed: " + response.error);
        }
    }
    if (!collectives_.barrier(round_tag + ".sync", timeout)) {
        return fail("distributed sort: shuffle barrier failed");
    }

    // 5. Merge our own bucket with everything received, then local-sort the union.
    std::vector<HostFrame> received;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto it = inbox_.find(shuffle_tag);
        if (it != inbox_.end()) {
            received.reserve(it->second.size());
            for (const PartitionMessage& message : it->second) {
                received.push_back(from_message(message));
            }
            inbox_.erase(it);
        }
    }
    received.push_back(std::move(buckets[rank]));

    std::vector<const HostFrame*> pointers;
    pointers.reserve(received.size());
    for (const HostFrame& frame : received) pointers.push_back(&frame);
    const std::optional<HostFrame> merged = internal::concat_host(pointers);
    if (!merged) return fail("distributed sort: received partitions disagree on schema");

    HostFrame output;
    if (const ComputeStatus status =
            internal::sort_host(device_, *merged, key_column, order, output);
        !status.ok()) {
        return {DataFrame{}, status};
    }
    DataFrameResult result = internal::upload_frame(device_, output, chunk_rows);
    return {std::move(result.frame), result.status};
}

}  // namespace zetta::ddf
