#include "zetta/data/virtual_table.hpp"

#include <utility>

namespace zetta::data {

std::string_view to_string(VirtualColumnTier tier) {
    switch (tier) {
        case VirtualColumnTier::Durable:         return "durable";
        case VirtualColumnTier::EphemeralLocal:  return "ephemeral-local";
        case VirtualColumnTier::EphemeralSynced: return "ephemeral-synced";
    }
    return "durable";
}

std::optional<VirtualColumnTier> virtual_column_tier_from(std::string_view text) {
    if (text == "durable")          return VirtualColumnTier::Durable;
    if (text == "ephemeral-local")  return VirtualColumnTier::EphemeralLocal;
    if (text == "ephemeral-synced") return VirtualColumnTier::EphemeralSynced;
    return std::nullopt;
}

std::string VirtualTable::key_column() const {
    for (const VirtualColumn& column : columns) {
        if (column.is_key) return column.name;
    }
    return {};
}

nlohmann::json virtual_table_to_json(const VirtualTable& table) {
    nlohmann::json columns = nlohmann::json::array();
    for (const VirtualColumn& column : table.columns) {
        columns.push_back(nlohmann::json{
            {"name", column.name},
            {"type", column.type},
            {"tier", std::string(to_string(column.tier))},
            {"key", column.is_key},
        });
    }
    return nlohmann::json{
        {"name", table.name},
        {"prefix", table.crdt_prefix},
        {"columns", std::move(columns)},
    };
}

std::optional<VirtualTable> virtual_table_from_json(
    const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    if (!document.contains("name") || !document["name"].is_string()) {
        return std::nullopt;
    }
    if (!document.contains("prefix") || !document["prefix"].is_string()) {
        return std::nullopt;
    }
    if (!document.contains("columns") || !document["columns"].is_array()) {
        return std::nullopt;
    }
    VirtualTable table;
    table.name = document["name"].get<std::string>();
    table.crdt_prefix = document["prefix"].get<std::string>();
    for (const nlohmann::json& entry : document["columns"]) {
        if (!entry.is_object() || !entry.contains("name") ||
            !entry["name"].is_string()) {
            return std::nullopt;
        }
        VirtualColumn column;
        column.name = entry["name"].get<std::string>();
        column.type = entry.value("type", std::string{});
        const std::string tier = entry.value("tier", std::string{"durable"});
        const auto parsed = virtual_column_tier_from(tier);
        if (!parsed) return std::nullopt;  // unknown tier — reject the record
        column.tier = *parsed;
        column.is_key = entry.value("key", false);
        table.columns.push_back(std::move(column));
    }
    return table;
}

}  // namespace zetta::data
