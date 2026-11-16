// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// A user-declared VIRTUAL TABLE over a CRDT key-family (#809 slice 3).
//
// The defining invariant: a virtual table MUST extend a CRDT "table" — the
// DURABLE tier is the convergent row identity; the ephemeral tiers are
// enrichment only. Its rows are computed at query time from the replicated
// document: one row per CRDT entry under `crdt_prefix`, the DURABLE KEY column
// carrying the id after the prefix, other DURABLE columns projected from the
// entry JSON, EPHEMERAL columns null until a resolver binds them (a follow-up).
//
// The definition itself is durable, replicated catalog metadata — stored under
// the `vtable:<name>` CRDT key (see src/core/catalog.hpp), so it converges and
// survives restart like db:/tbl:. Read-only: writes go to the base entity's own
// path. See docs/data/virtual-tables.md.
namespace zetta::data {

// The tier a virtual-table column draws from. DURABLE columns are fields of the
// CRDT entry (replicated); EPHEMERAL columns are enrichment — telemetry
// (synced) or computed local state (local) — null until a resolver is bound.
enum class VirtualColumnTier { Durable, EphemeralLocal, EphemeralSynced };

std::string_view to_string(VirtualColumnTier tier);
std::optional<VirtualColumnTier> virtual_column_tier_from(std::string_view text);

// One column of a virtual table.
struct VirtualColumn {
    std::string name;
    std::string type;   // captured, not enforced (the storage is schemaless JSON)
    VirtualColumnTier tier = VirtualColumnTier::Durable;
    bool is_key = false;  // DURABLE KEY — the CRDT key (the id after the prefix)

    bool operator==(const VirtualColumn&) const = default;
};

// A virtual-table definition. `crdt_prefix` is the REQUIRED key-family prefix
// (the must-extend-a-CRDT invariant); `columns` are its tiered columns, exactly
// one of which is the DURABLE KEY.
struct VirtualTable {
    std::string name;
    std::string crdt_prefix;
    std::vector<VirtualColumn> columns;

    bool operator==(const VirtualTable&) const = default;

    // The DURABLE KEY column's name, or empty when none is marked.
    std::string key_column() const;
};

// Serialize a definition to its `vtable:<name>` CRDT value.
nlohmann::json virtual_table_to_json(const VirtualTable& table);

// Parse a `vtable:` CRDT value back into a definition (non-throwing boundary):
// returns nullopt on a structurally invalid document.
std::optional<VirtualTable> virtual_table_from_json(const nlohmann::json& document);

}  // namespace zetta::data
