// SPDX-License-Identifier: MIT

#pragma once

// The text entity-key table (#1255, docs/data/entity-key-table.md).
//
// A text entity column is keyed for the numeric engine by a derived id (a stable
// hash of the text). Restoring the business id needs the reverse map, one entry
// per distinct id in the dataset. That map used to be stored inline in the Run
// record and in the Model's feature_schema, so every replicated write, every
// peer apply and every system.runs / system.models row carried every id.
//
// It is now a table: rows of the engine-internal record-store collection
// `_entity_keys`, keyed by (run or model, chunk), each chunk holding up to
// kEntityKeyEntriesPerChunk entries in derived-id order. The record keeps only a
// reference:
//
//   {"column": "customer_id", "entry_count": 1500, "table": "_entity_keys",
//    "owner": "model:<uuid>", "chunks": 2}
//
// The API refuses collection names starting with `_`, so the rows are reached
// only through the run or model that references them: get_run / get_model with
// include_entity_keys, score, predict. Each of those checks the caller may read
// that run or model first.
//
// Records written before #1255 keep the inline form `{column, entries}`, and
// every reader accepts both (migration on read).
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"

namespace zetta::data {

class RecordStore;

inline constexpr std::string_view kEntityKeyCollection = "_entity_keys";
inline constexpr std::size_t kEntityKeyEntriesPerChunk = 1024;

// The owner of a run's or a model's rows: "run:<id>", "model:<id>".
std::string entity_key_owner(std::string_view kind, std::string_view id);

// A reference to the table (it names `table`), as opposed to the inline form.
bool is_entity_key_reference(const nlohmann::json& mapping);

// The JSON Schema of a reference.
const nlohmann::json& entity_key_reference_schema();

// Write `mapping`'s entries as `owner`'s rows and return the reference to put in
// the record. Rows a previous write left under the same owner are replaced.
// nullopt with `error` when a row could not be written.
std::optional<nlohmann::json> store_entity_key_table(RecordStore& store, std::string_view owner,
                                                     const EntityKeyMapping& mapping,
                                                     std::string* error = nullptr);

// Either form to a mapping. The inline form is parsed as before. A reference is
// read from this node's rows; when some have not reached this node yet (a peer
// can hold the Model before anti-entropy brings the rows) it fails with a reason
// naming what is missing, and never answers with ids it cannot restore. `store`
// may be null (a standalone node), which only the inline form survives.
std::shared_ptr<const EntityKeyMapping> load_entity_key_mapping(const RecordStore* store,
                                                                const nlohmann::json& mapping,
                                                                std::string* error = nullptr);

// Remove `owner`'s rows when `mapping` is a reference that names `owner`; a
// reference shared from another owner, and the inline form, are left alone.
void drop_entity_key_table(RecordStore& store, const nlohmann::json& mapping,
                           std::string_view owner);

// get_run / get_model with include_entity_keys: replace every reference under an
// `entity_key_mapping` key anywhere in `document` by the inline form. False with
// `error` when one cannot be resolved.
using EntityKeyResolver = std::function<std::shared_ptr<const EntityKeyMapping>(
    const nlohmann::json& mapping, std::string* error)>;
bool expand_entity_key_mappings(nlohmann::json& document, const EntityKeyResolver& resolve,
                                std::string* error = nullptr);

}  // namespace zetta::data
