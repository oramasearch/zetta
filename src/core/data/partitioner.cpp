// SPDX-License-Identifier: MIT

#include "zetta/data/partitioner.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::data {

namespace {

// A stable 64-bit FNV-1a hash of a byte string. "Stable" is the load-bearing
// property: the partition a key maps to must be identical on every node and
// across restarts (durable placement), so std::hash — whose value is unspecified
// and varies across standard-library implementations and even runs — cannot be
// used. FNV-1a is the small, deterministic, well-distributed hash zetta already
// uses for content fingerprints (src/core/replication.cpp, gbdt_train_job.cpp).
std::uint64_t fnv1a_64(const std::string& bytes) {
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a (64-bit) offset basis
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;  // FNV-1a prime
    }
    return hash;
}

// Build the byte string to hash from a record's key columns, in order. Each
// column's value is dumped to its canonical JSON text (so "5" the string and 5
// the number don't collide) and the dumps are joined by a unit separator (so
// ["a","b"]=("x","yz") and ("xy","z") don't collide). A missing column hashes as
// JSON null. An empty column list (a degenerate config) hashes the whole record.
std::string hash_key(const nlohmann::json& record,
                     const std::vector<std::string>& key_columns) {
    if (key_columns.empty()) {
        return record.dump();
    }
    std::string key;
    for (const std::string& column : key_columns) {
        key.push_back('\x1f');  // unit separator
        const nlohmann::json value =
            record.is_object() ? record.value(column, nlohmann::json())
                               : nlohmann::json();
        key += value.dump();
    }
    return key;
}

// Read an integer surrogate from a record field, sign-safely folded into [0, P).
// A missing/non-integer field returns partition 0 (in range, deterministic).
PartitionId surrogate_partition(const nlohmann::json& record,
                                const std::string& field, PartitionId count) {
    if (!record.is_object() || !record.contains(field) ||
        !record.at(field).is_number_integer()) {
        return 0;
    }
    const std::int64_t surrogate = record.at(field).get<std::int64_t>();
    const std::int64_t modulus = static_cast<std::int64_t>(count);
    const std::int64_t bucket = ((surrogate % modulus) + modulus) % modulus;
    return static_cast<PartitionId>(bucket);
}

void fail(std::string* error, std::string reason) {
    if (error != nullptr) *error = std::move(reason);
}

}  // namespace

PartitionId HashPartitioner::assign_partition(const nlohmann::json& record) const {
    return static_cast<PartitionId>(fnv1a_64(hash_key(record, key_columns_)) %
                                    partition_count_);
}

nlohmann::json HashPartitioner::to_json() const {
    return nlohmann::json{{"strategy", "hash"},
                          {"partition_count", partition_count_},
                          {"key_columns", key_columns_}};
}

PartitionId RoundRobinPartitioner::assign_partition(
    const nlohmann::json& record) const {
    return surrogate_partition(record, surrogate_field_, partition_count_);
}

nlohmann::json RoundRobinPartitioner::to_json() const {
    return nlohmann::json{{"strategy", "round_robin"},
                          {"partition_count", partition_count_},
                          {"surrogate_field", surrogate_field_}};
}

std::shared_ptr<Partitioner> partitioner_from_json(const nlohmann::json& value,
                                                   std::string* error) {
    if (!value.is_object()) {
        fail(error, "partitioner config must be a JSON object");
        return nullptr;
    }
    if (!value.contains("strategy") || !value["strategy"].is_string()) {
        fail(error, "partitioner config missing string 'strategy'");
        return nullptr;
    }
    const std::string strategy = value["strategy"].get<std::string>();

    // partition_count: required for the sharded strategies, defaults to 1 for the
    // trivial single. Accept signed-but-non-negative or unsigned JSON integers
    // (the shape ScalingPolicy/NodeCapability accept), and require >= 1.
    PartitionId partition_count = 1;
    if (value.contains("partition_count")) {
        const nlohmann::json& count = value["partition_count"];
        std::int64_t signed_count = 0;
        if (count.is_number_unsigned()) {
            signed_count = static_cast<std::int64_t>(count.get<std::uint64_t>());
        } else if (count.is_number_integer()) {
            signed_count = count.get<std::int64_t>();
        } else {
            fail(error, "'partition_count' must be an integer");
            return nullptr;
        }
        if (signed_count < 1 ||
            signed_count > static_cast<std::int64_t>(
                               std::numeric_limits<PartitionId>::max())) {
            fail(error, "'partition_count' must be in [1, 2^32)");
            return nullptr;
        }
        partition_count = static_cast<PartitionId>(signed_count);
    }

    if (strategy == "single") {
        if (partition_count != 1) {
            fail(error, "single partitioner must have partition_count 1");
            return nullptr;
        }
        return std::make_shared<SinglePartitioner>();
    }

    if (strategy == "hash") {
        if (!value.contains("partition_count")) {
            fail(error, "hash partitioner requires 'partition_count'");
            return nullptr;
        }
        if (!value.contains("key_columns") || !value["key_columns"].is_array() ||
            value["key_columns"].empty()) {
            fail(error, "hash partitioner requires a non-empty 'key_columns' array");
            return nullptr;
        }
        std::vector<std::string> key_columns;
        std::unordered_set<std::string> seen;
        for (const nlohmann::json& column : value["key_columns"]) {
            if (!column.is_string() || column.get<std::string>().empty()) {
                fail(error, "'key_columns' entries must be non-empty strings");
                return nullptr;
            }
            std::string name = column.get<std::string>();
            if (!seen.insert(name).second) {
                fail(error, "'key_columns' entries must be unique");
                return nullptr;
            }
            key_columns.push_back(std::move(name));
        }
        return std::make_shared<HashPartitioner>(partition_count,
                                                 std::move(key_columns));
    }

    if (strategy == "round_robin") {
        if (!value.contains("partition_count")) {
            fail(error, "round_robin partitioner requires 'partition_count'");
            return nullptr;
        }
        std::string surrogate_field = "_seq";
        if (value.contains("surrogate_field")) {
            if (!value["surrogate_field"].is_string() ||
                value["surrogate_field"].get<std::string>().empty()) {
                fail(error, "'surrogate_field' must be a non-empty string");
                return nullptr;
            }
            surrogate_field = value["surrogate_field"].get<std::string>();
        }
        return std::make_shared<RoundRobinPartitioner>(partition_count,
                                                       std::move(surrogate_field));
    }

    fail(error, "unknown partitioner strategy '" + strategy + "'");
    return nullptr;
}

// The Partitioner config JSON Schema (draft 2020-12) — the code-owned source of truth;
// schemas/partitioner.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& partitioner_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/partitioner.schema.json",
  "title": "Partitioner",
  "description": "The serialized configuration of a table's partitioner — the per-table policy that maps a record to one of P fixed partitions (ADR-31 / ENT-37; #512, #513). The 'strategy' selects the record→partition implementation: 'single' (the trivial baseline — one partition holding every row), 'hash' (hash of key columns % P, deterministic shard-by-key), or 'round_robin' (a monotonic surrogate % P, for even balance regardless of key skew). Mirrored by zetta::Partitioner::to_json() / partitioner_from_json() in include/zetta/partitioner.hpp + src/core/partitioner.cpp — keep them in sync. A malformed config is rejected at the boundary so a table is never configured with a partitioner that would mis-place rows. The partition→node placement half (RendezvousPlacement, #514) and persisting this config in the partition catalog (#510) / write path (#511) are dependent slices.",
  "type": "object",
  "required": ["strategy"],
  "additionalProperties": false,
  "properties": {
    "strategy": {
      "description": "Which record→partition implementation. The lowercase name is the stored identifier (matches Partitioner::name()).",
      "type": "string",
      "enum": ["single", "hash", "round_robin"]
    },
    "partition_count": {
      "description": "The fixed partition count P (chosen P >> nodes), constant for the life of the partitioner. Required (>= 1) for 'hash' and 'round_robin'; for 'single' it is always 1 (omitted or 1).",
      "type": "integer",
      "minimum": 1
    },
    "key_columns": {
      "description": "The record field(s) to hash, in order — required and non-empty for the 'hash' strategy, absent otherwise. Each entry is a non-empty field name and the entries are unique.",
      "type": "array",
      "minItems": 1,
      "uniqueItems": true,
      "items": {
        "type": "string",
        "minLength": 1
      }
    },
    "surrogate_field": {
      "description": "The record field holding the monotonic integer surrogate the 'round_robin' strategy takes modulo P — defaults to '_seq' when omitted, absent for other strategies. Non-empty when present.",
      "type": "string",
      "minLength": 1
    }
  },
  "allOf": [
    {
      "if": { "properties": { "strategy": { "const": "hash" } } },
      "then": { "required": ["partition_count", "key_columns"] }
    },
    {
      "if": { "properties": { "strategy": { "const": "round_robin" } } },
      "then": { "required": ["partition_count"] }
    }
  ]
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::data