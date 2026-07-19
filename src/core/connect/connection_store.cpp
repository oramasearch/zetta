#include "zetta/connect/connection_store.hpp"

#include <cctype>
#include <utility>

#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::connect {
namespace {

// The reserved table, or an empty handle when nothing has been saved yet (the
// database is created lazily, on the first save, so a node that never uses a
// connector carries no extra schema).
std::shared_ptr<data::Table> find_store_table(const Node& node) {
    const std::shared_ptr<data::Database> database =
        node.find_database(kConnectionDatabase);
    if (!database) return {};
    return database->find_table(kConnectionTable);
}

// Read one stored row back into a SavedConnection. Returns nullopt for a row
// that does not parse (a hand-edited row, or one written by a newer build) —
// never throws.
std::optional<SavedConnection> connection_from_row(const nlohmann::json& row) {
    if (!row.is_object()) return std::nullopt;
    const auto name = row.find("name");
    if (name == row.end() || !name->is_string() ||
        name->get<std::string>().empty()) {
        return std::nullopt;
    }
    const auto connector = row.find("connector");
    if (connector == row.end()) return std::nullopt;
    const std::optional<ConnectorConfig> config =
        ConnectorConfig::from_json(*connector);
    if (!config) return std::nullopt;

    SavedConnection saved;
    saved.name = name->get<std::string>();
    saved.config = *config;
    const auto created = row.find("created");
    if (created != row.end() && created->is_string()) {
        saved.created = created->get<std::string>();
    }
    return saved;
}

// The row id of the connection called `name`, or empty when absent. A row's id
// is the store's key, not a field in the document (`at(i)` is data only), so it
// comes from `id_at(i)`.
std::string row_id_of(const data::Table& table, const std::string& name) {
    const zetta::storage::StorageEngine& store = table.store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const nlohmann::json& row = store.at(index);
        if (!row.is_object()) continue;
        const auto stored = row.find("name");
        if (stored == row.end() || !stored->is_string()) continue;
        if (stored->get<std::string>() == name) return store.id_at(index);
    }
    return {};
}

}  // namespace

nlohmann::json SavedConnection::to_redacted_json() const {
    // to_json() never emits the token, so the redaction is structural rather
    // than a field we remember to strip.
    nlohmann::json value;
    value["name"] = name;
    value["connector"] = config.to_json();
    value["created"] = created;
    return value;
}

std::string connection_name_rejection(const std::string& name) {
    if (name.empty()) return "connection: a name is required";
    if (name.size() > 64) {
        return "connection: a name may be at most 64 characters";
    }
    if (!std::isalnum(static_cast<unsigned char>(name.front()))) {
        return "connection: a name must start with a letter or digit";
    }
    for (const char character : name) {
        const bool allowed =
            std::isalnum(static_cast<unsigned char>(character)) ||
            character == '_' || character == '-';
        if (!allowed) {
            return "connection: a name may contain only letters, digits, '_' "
                   "and '-'";
        }
    }
    return {};
}

std::string save_connection(Node& node, const std::string& name,
                            const ConnectorConfig& config) {
    const std::string rejection = connection_name_rejection(name);
    if (!rejection.empty()) return rejection;
    if (config.token.empty()) {
        return "connection: the connector config carries no token";
    }

    std::shared_ptr<data::Database> database =
        node.find_database(kConnectionDatabase);
    if (!database) database = node.create_database(kConnectionDatabase);
    if (!database) {
        return "connection: could not create the '" +
               std::string(kConnectionDatabase) + "' database";
    }
    std::shared_ptr<data::Table> table = database->find_table(kConnectionTable);
    // A plain table: its ReplicationStrategy defaults to ReplicateToAll, which
    // is exactly what a credential every node must present needs. It is
    // deliberately NOT partitioned.
    if (!table) table = database->create_table(kConnectionTable);
    if (!table) {
        return "connection: could not create the '" +
               std::string(kConnectionTable) + "' table";
    }

    nlohmann::json row;
    row["name"] = name;
    row["connector"] = config.to_storage_json();  // the ONE token-carrying form
    row["created"] = zetta::util::now_iso8601();

    // Replace in place when the name already exists, so saving twice does not
    // leave two rows racing to answer the same lookup.
    const std::string existing = row_id_of(*table, name);
    if (!existing.empty()) {
        table->update_row(existing, std::move(row));
    } else {
        table->add_row(std::move(row));
    }
    return {};
}

std::optional<SavedConnection> find_connection(const Node& node,
                                               const std::string& name) {
    const std::shared_ptr<data::Table> table = find_store_table(node);
    if (!table) return std::nullopt;
    const zetta::storage::StorageEngine& store = table->store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const std::optional<SavedConnection> saved =
            connection_from_row(store.at(index));
        if (saved && saved->name == name) return saved;
    }
    return std::nullopt;
}

std::vector<SavedConnection> list_connections(const Node& node) {
    std::vector<SavedConnection> connections;
    const std::shared_ptr<data::Table> table = find_store_table(node);
    if (!table) return connections;
    const zetta::storage::StorageEngine& store = table->store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        std::optional<SavedConnection> saved =
            connection_from_row(store.at(index));
        if (saved) connections.push_back(std::move(*saved));
    }
    return connections;
}

bool remove_connection(Node& node, const std::string& name) {
    const std::shared_ptr<data::Table> table = find_store_table(node);
    if (!table) return false;
    const std::string existing = row_id_of(*table, name);
    if (existing.empty()) return false;
    table->delete_row(existing);
    return true;
}

}  // namespace zetta::connect
