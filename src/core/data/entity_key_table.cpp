// SPDX-License-Identifier: MIT

// The text entity-key table (#1255): a run's or a model's derived-id map as rows
// of the engine-internal record collection `_entity_keys`, referenced from the
// record. See include/zetta/data/entity_key_table.hpp and
// docs/data/entity-key-table.md.
#include "zetta/data/entity_key_table.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

#include "zetta/data/record_store.hpp"

namespace zetta::data {

namespace {

const std::string kCollection{kEntityKeyCollection};

std::string chunk_key(std::string_view owner, std::size_t chunk) {
    return std::format("{}:{:06}", owner, chunk);
}

std::string chunk_prefix(std::string_view owner) { return std::string(owner) + ":"; }

// Every key `owner` holds, in key (so chunk) order.
std::vector<std::pair<std::string, std::string>> owner_rows(const RecordStore& store,
                                                           std::string_view owner) {
    const std::string prefix = chunk_prefix(owner);
    std::vector<std::pair<std::string, std::string>> rows;
    store.scan(kCollection, prefix, [&](const std::string& key, const std::string& value) {
        if (!key.starts_with(prefix)) return false;
        rows.emplace_back(key, value);
        return true;
    });
    return rows;
}

bool fail(std::string* error, std::string reason) {
    if (error != nullptr) *error = std::move(reason);
    return false;
}

}  // namespace

std::string entity_key_owner(std::string_view kind, std::string_view id) {
    return std::string(kind) + ":" + std::string(id);
}

bool is_entity_key_reference(const nlohmann::json& mapping) {
    return mapping.is_object() && mapping.contains("table");
}

const nlohmann::json& entity_key_reference_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"column", "entry_count", "table", "owner", "chunks"}},
        {"additionalProperties", false},
        {"description",
         "a text entity-key mapping stored as a table (#1255): the entity column, how "
         "many distinct ids it maps, the engine-internal table holding them, the owner "
         "whose rows they are (run:<id> or model:<id>) and how many rows"},
        {"properties",
         {{"column", {{"type", "string"}, {"minLength", 1}}},
          {"entry_count", {{"type", "integer"}, {"minimum", 1}}},
          {"table", {{"const", std::string(kEntityKeyCollection)}}},
          {"owner", {{"type", "string"}, {"minLength", 1}}},
          {"chunks", {{"type", "integer"}, {"minimum", 1}}}}}};
    return schema;
}

std::optional<nlohmann::json> store_entity_key_table(RecordStore& store, std::string_view owner,
                                                     const EntityKeyMapping& mapping,
                                                     std::string* error) {
    if (mapping.empty() || mapping.column.empty() || owner.empty()) {
        fail(error, "entity key table: want a column, an owner and at least one entry");
        return std::nullopt;
    }
    std::size_t chunk = 0;
    nlohmann::json entries = nlohmann::json::array();
    const auto flush = [&]() -> bool {
        const std::string key = chunk_key(owner, chunk);
        if (!store.put_internal(kCollection, key, nlohmann::json{{"entries", entries}}.dump())) {
            return fail(error, "entity key table: could not write row " + key);
        }
        ++chunk;
        entries = nlohmann::json::array();
        return true;
    };
    for (const auto& [derived_id, original] : mapping.derived_to_original) {
        entries.push_back({derived_id, original});
        if (entries.size() == kEntityKeyEntriesPerChunk && !flush()) return std::nullopt;
    }
    if (!entries.empty() && !flush()) return std::nullopt;
    // Rows beyond the new count are left over from an earlier, larger mapping.
    for (const auto& [key, value] : owner_rows(store, owner)) {
        if (key > chunk_key(owner, chunk - 1)) store.remove_internal(kCollection, key);
    }
    return nlohmann::json{{"column", mapping.column},
                          {"entry_count", mapping.derived_to_original.size()},
                          {"table", kCollection},
                          {"owner", std::string(owner)},
                          {"chunks", chunk}};
}

std::shared_ptr<const EntityKeyMapping> load_entity_key_mapping(const RecordStore* store,
                                                                const nlohmann::json& mapping,
                                                                std::string* error) {
    if (!is_entity_key_reference(mapping)) return entity_key_mapping_from_json(mapping, error);
    const auto refuse = [error](std::string reason) {
        fail(error, std::move(reason));
        return std::shared_ptr<const EntityKeyMapping>{};
    };
    const auto text = [&](std::string_view field) -> std::string {
        const auto found = mapping.find(field);
        return found != mapping.end() && found->is_string() ? found->get<std::string>()
                                                            : std::string();
    };
    const auto count = [&](std::string_view field) -> std::size_t {
        const auto found = mapping.find(field);
        if (found == mapping.end() || !found->is_number_integer()) return 0;
        const auto value = found->get<std::int64_t>();
        return value > 0 ? static_cast<std::size_t>(value) : 0;
    };
    const std::string column = text("column");
    const std::string owner = text("owner");
    const std::size_t entry_count = count("entry_count");
    const std::size_t chunks = count("chunks");
    if (column.empty() || owner.empty() || entry_count == 0 || chunks == 0 ||
        text("table") != kEntityKeyCollection) {
        return refuse("entity key mapping: a reference needs a column, an owner, the table " +
                      kCollection + ", an entry_count and a chunk count");
    }
    if (store == nullptr) {
        return refuse("entity key mapping: the ids of '" + column + "' are stored in the " +
                      kCollection + " table, and this node has no record store to read it");
    }
    auto result = std::make_shared<EntityKeyMapping>();
    result->column = column;
    const auto rows = owner_rows(*store, owner);
    std::size_t expected = 0;
    for (const auto& [key, value] : rows) {
        if (key != chunk_key(owner, expected)) break;
        const nlohmann::json row = nlohmann::json::parse(value, nullptr, false);
        const auto entries = row.is_object() ? row.find("entries") : row.end();
        if (row.is_discarded() || !row.is_object() || entries == row.end() ||
            !entries->is_array()) {
            return refuse("entity key mapping: row " + key + " of " + kCollection +
                          " is unreadable");
        }
        for (const nlohmann::json& entry : *entries) {
            if (!entry.is_array() || entry.size() != 2 || !entry[0].is_number_integer() ||
                !entry[1].is_string() || entry[1].get_ref<const std::string&>().empty()) {
                return refuse("entity key mapping: row " + key + " of " + kCollection +
                              " has a malformed entry");
            }
            const auto derived_id = entry[0].get<std::int64_t>();
            const std::string& original = entry[1].get_ref<const std::string&>();
            if (!result->derived_to_original.emplace(derived_id, original).second ||
                !result->original_to_derived.emplace(original, derived_id).second) {
                return refuse("entity key mapping: " + owner +
                              " holds a duplicate key or value");
            }
        }
        ++expected;
    }
    if (expected < chunks || result->derived_to_original.size() != entry_count) {
        return refuse(std::format(
            "entity key mapping: {} of the {} rows of {}'s ids for '{}' ({} of {} ids) are on "
            "this node; the rest have not reached it yet, so its ids cannot be restored; retry "
            "once replication catches up",
            expected, chunks, owner, column, result->derived_to_original.size(), entry_count));
    }
    return result;
}

void drop_entity_key_table(RecordStore& store, const nlohmann::json& mapping,
                           std::string_view owner) {
    if (!is_entity_key_reference(mapping) || mapping.value("owner", std::string()) != owner) {
        return;
    }
    for (const auto& [key, value] : owner_rows(store, owner)) {
        store.remove_internal(kCollection, key);
    }
}

bool expand_entity_key_mappings(nlohmann::json& document, const EntityKeyResolver& resolve,
                                std::string* error) {
    if (document.is_object()) {
        for (auto& [key, value] : document.items()) {
            if (key == "entity_key_mapping" && is_entity_key_reference(value)) {
                const std::shared_ptr<const EntityKeyMapping> mapping = resolve(value, error);
                if (!mapping) return false;
                value = mapping->to_json();
            } else if (!expand_entity_key_mappings(value, resolve, error)) {
                return false;
            }
        }
    } else if (document.is_array()) {
        for (nlohmann::json& item : document) {
            if (!expand_entity_key_mappings(item, resolve, error)) return false;
        }
    }
    return true;
}

}  // namespace zetta::data
