// SPDX-License-Identifier: MIT

#include "zetta/governance/safe_mode.hpp"

#include <utility>

#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/credential_redaction.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::governance {
namespace {

std::shared_ptr<data::Table> find_store_table(const Node& node) {
    const std::shared_ptr<data::Database> database =
        node.find_database(kSafeModeDatabase);
    if (!database) return {};
    return database->find_table(kSafeModeTable);
}

// One stored level, read back from a row. nullopt for a row that does not
// parse (written by a newer build, or edited by hand) — never trusted.
struct StoredLevel {
    std::string row_id;
    SafeMode mode = SafeMode::WarnWrites;
    std::string updated;
};

bool row_names(const nlohmann::json& row, SafeModeTarget target,
               const std::string& name) {
    if (!row.is_object()) return false;
    const auto stored_target = row.find("target");
    const auto stored_name = row.find("name");
    return stored_target != row.end() && stored_target->is_string() &&
           stored_target->get<std::string>() == to_string(target) &&
           stored_name != row.end() && stored_name->is_string() &&
           stored_name->get<std::string>() == name;
}

// Every row for one database. Two nodes setting a level at the same moment can
// each add a row; the latest `updated` wins on read, and the next write
// replaces them all.
std::vector<StoredLevel> stored_levels(const data::Table& table,
                                       SafeModeTarget target,
                                       const std::string& name) {
    std::vector<StoredLevel> levels;
    const zetta::storage::StorageEngine& store = table.store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row || !row_names(*row, target, name)) continue;
        const auto mode = row->find("mode");
        if (mode == row->end() || !mode->is_string()) continue;
        const std::optional<SafeMode> parsed =
            parse_safe_mode(mode->get<std::string>());
        if (!parsed) continue;
        levels.push_back({store.id_at(index), *parsed,
                          row->value("updated", std::string())});
    }
    return levels;
}

}  // namespace

std::string_view to_string(SafeMode mode) {
    switch (mode) {
        case SafeMode::Off: return "off";
        case SafeMode::WarnWrites: return "warn_writes";
        case SafeMode::WarnAll: return "warn_all";
    }
    return "warn_writes";
}

std::optional<SafeMode> parse_safe_mode(std::string_view text) {
    if (text == "off") return SafeMode::Off;
    if (text == "warn_writes") return SafeMode::WarnWrites;
    if (text == "warn_all") return SafeMode::WarnAll;
    return std::nullopt;
}

std::string_view to_string(SafeModeTarget target) {
    return target == SafeModeTarget::Connection ? "connection" : "database";
}

SafeMode default_safe_mode(SafeModeTarget target) {
    return target == SafeModeTarget::Connection ? SafeMode::WarnWrites
                                                : SafeMode::Off;
}

std::optional<SafeMode> stored_safe_mode(const Node& node, SafeModeTarget target,
                                         const std::string& name) {
    const std::shared_ptr<data::Table> table = find_store_table(node);
    if (!table) return std::nullopt;
    const std::vector<StoredLevel> levels = stored_levels(*table, target, name);
    if (levels.empty()) return std::nullopt;
    const StoredLevel* latest = &levels.front();
    for (const StoredLevel& level : levels) {
        // ISO-8601 UTC stamps order lexicographically. A tie is broken towards
        // the stricter level, so a race never loosens a database.
        if (level.updated > latest->updated ||
            (level.updated == latest->updated && level.mode > latest->mode)) {
            latest = &level;
        }
    }
    return latest->mode;
}

SafeMode effective_safe_mode(const Node& node, SafeModeTarget target,
                             const std::string& name) {
    return stored_safe_mode(node, target, name).value_or(default_safe_mode(target));
}

std::string store_safe_mode(Node& node, SafeModeTarget target,
                            const std::string& name, SafeMode mode,
                            const std::string& actor) {
    if (name.empty()) return "safe mode: a database or connection name is required";
    std::shared_ptr<data::Database> database = node.find_database(kSafeModeDatabase);
    if (!database) database = node.create_database(kSafeModeDatabase);
    if (!database) {
        return "safe mode: could not create the '" + std::string(kSafeModeDatabase) +
               "' database";
    }
    // A plain table: its ReplicationStrategy defaults to ReplicateToAll, so every
    // node that may run a statement holds every level.
    std::shared_ptr<data::Table> table = database->find_table(kSafeModeTable);
    if (!table) table = database->create_table(kSafeModeTable);
    if (!table) {
        return "safe mode: could not create the '" + std::string(kSafeModeTable) +
               "' table";
    }
    nlohmann::json row{{"target", std::string(to_string(target))},
                       {"name", name},
                       {"mode", std::string(to_string(mode))},
                       {"updated", zetta::util::now_iso8601()},
                       {"updated_by", actor.empty() ? std::string("local") : actor}};
    std::vector<StoredLevel> levels = stored_levels(*table, target, name);
    if (levels.empty()) {
        table->add_row(std::move(row));
        return {};
    }
    // Replace in place, and drop any duplicate a concurrent write left behind.
    table->update_row(levels.front().row_id, std::move(row));
    for (std::size_t index = 1; index < levels.size(); ++index) {
        table->delete_row(levels[index].row_id);
    }
    return {};
}

bool clear_safe_mode(Node& node, SafeModeTarget target, const std::string& name) {
    const std::shared_ptr<data::Table> table = find_store_table(node);
    if (!table) return false;
    const std::vector<StoredLevel> levels = stored_levels(*table, target, name);
    for (const StoredLevel& level : levels) table->delete_row(level.row_id);
    return !levels.empty();
}

bool safe_mode_warns(SafeMode mode, const SafeModeStatements& statements) {
    if (statements.statements.empty()) return false;
    switch (mode) {
        case SafeMode::Off: return false;
        case SafeMode::WarnWrites: return statements.writes;
        case SafeMode::WarnAll: return true;
    }
    return true;
}

std::string safe_mode_confirmation(const std::vector<SafeModeStatements>& warned) {
    // Length-prefixed fields, so no choice of names or statements can make two
    // different requests hash the same bytes.
    std::string material = "zetta-safe-mode/1";
    const auto field = [&](std::string_view text) {
        material += '\n';
        material += std::to_string(text.size());
        material += ':';
        material.append(text);
    };
    for (const SafeModeStatements& entry : warned) {
        field(to_string(entry.target));
        field(entry.name);
        field(std::to_string(entry.statements.size()));
        for (const std::string& statement : entry.statements) field(statement);
    }
    return zetta::util::sha256_hex(material);
}

std::string redact_secret(std::string statement, std::string_view secret) {
    if (secret.size() < 4) return statement;
    std::size_t at = 0;
    while ((at = statement.find(secret, at)) != std::string::npos) {
        statement.replace(at, secret.size(), kRedactedCredential);
        at += kRedactedCredential.size();
    }
    return statement;
}

}  // namespace zetta::governance
