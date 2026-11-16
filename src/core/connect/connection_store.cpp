// SPDX-License-Identifier: MIT

#include "zetta/connect/connection_store.hpp"

#include "zetta/connect/credential_seal.hpp"

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

// Read one stored row back into a SavedConnection, opening its sealed
// credential with the node's keyring. Returns nullopt for a row that does not
// parse (a hand-edited row, or one written by a newer build) — never throws. A
// credential that does not open still yields the connection (its metadata is
// listable), marked Unreadable with an empty token.
std::optional<SavedConnection> connection_from_row(const Node& node,
                                                   const nlohmann::json& row) {
    if (!row.is_object()) return std::nullopt;
    const auto name = row.find("name");
    if (name == row.end() || !name->is_string() ||
        name->get<std::string>().empty()) {
        return std::nullopt;
    }
    const auto connector = row.find("connector");
    if (connector == row.end() || !connector->is_object()) return std::nullopt;

    SavedConnection saved;
    saved.name = name->get<std::string>();
    nlohmann::json settings = *connector;
    std::string token;
    const auto sealed = row.find("credential");
    if (sealed != row.end()) {
        if (settings.contains("token")) return std::nullopt;  // both forms: corrupt
        std::string reason;
        const CredentialKeyring* keyring = node.connection_keyring(&reason);
        std::optional<std::string> opened;
        if (keyring) {
            opened = unseal_credential(*keyring, *sealed, saved.name, &reason);
        }
        if (opened) {
            token = std::move(*opened);
            saved.credential = CredentialState::Set;
        } else {
            saved.credential = CredentialState::Unreadable;
            saved.credential_error = reason;
        }
    } else if (const auto plain = settings.find("token");
               plain != settings.end() && plain->is_string() &&
               !plain->get<std::string>().empty()) {
        token = plain->get<std::string>();  // legacy, pre-#1230 row
        saved.credential = CredentialState::Plaintext;
    }
    // from_json enforces the per-type required fields, and a Snowflake config
    // requires a token. An Unreadable credential is parsed with a stand-in so
    // the connection's metadata stays listable; the stand-in never leaves this
    // function — the token is cleared below.
    const bool stand_in =
        saved.credential == CredentialState::Unreadable && token.empty();
    settings["token"] = stand_in ? std::string("unreadable") : token;
    std::optional<ConnectorConfig> config = ConnectorConfig::from_json(settings);
    if (!config) return std::nullopt;
    if (stand_in) config->token.clear();
    saved.config = std::move(*config);
    const auto created = row.find("created");
    if (created != row.end() && created->is_string()) {
        saved.created = created->get<std::string>();
    }
    return saved;
}

// The stored row for `name` + `config`: the settings without the token, and the
// token sealed under the node's current key (absent when there is none to
// seal). Empty optional with a reason when the node holds no sealing key.
std::optional<nlohmann::json> stored_row(const Node& node, const std::string& name,
                                         const ConnectorConfig& config,
                                         const std::string& created,
                                         std::string* reason) {
    nlohmann::json row;
    row["name"] = name;
    row["connector"] = config.to_json();  // never carries the token
    if (!config.token.empty()) {
        std::string why;
        const CredentialKeyring* keyring = node.connection_keyring(&why);
        std::optional<nlohmann::json> sealed;
        if (keyring) sealed = seal_credential(*keyring, config.token, name, &why);
        if (!sealed) {
            if (reason) *reason = "connection: cannot seal the credential: " + why;
            return std::nullopt;
        }
        row["credential"] = std::move(*sealed);
    }
    row["created"] = created;
    return row;
}

// The row id of the connection called `name`, or empty when absent. A row's id
// is the store's key, not a field in the document (`at(i)` is data only), so it
// comes from `id_at(i)`.
std::string row_id_of(const data::Table& table, const std::string& name) {
    const zetta::storage::StorageEngine& store = table.store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row || !row->is_object()) continue;
        const auto stored = row->find("name");
        if (stored == row->end() || !stored->is_string()) continue;
        if (stored->get<std::string>() == name) return store.id_at(index);
    }
    return {};
}

}  // namespace

std::string_view to_string(CredentialState state) {
    switch (state) {
        case CredentialState::Set: return "set";
        case CredentialState::None: return "none";
        case CredentialState::Unreadable: return "unreadable";
        case CredentialState::Plaintext: return "plaintext";
    }
    return "none";
}

nlohmann::json SavedConnection::to_redacted_json() const {
    // to_json() never emits the token, so the redaction is structural rather
    // than a field we remember to strip.
    nlohmann::json value;
    value["name"] = name;
    value["connector"] = config.to_json();
    value["credential"] = std::string(to_string(credential));
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

    std::string reason;
    std::optional<nlohmann::json> row =
        stored_row(node, name, config, zetta::util::now_iso8601(), &reason);
    if (!row) return reason;

    // Replace in place when the name already exists, so saving twice does not
    // leave two rows racing to answer the same lookup.
    const std::string existing = row_id_of(*table, name);
    if (!existing.empty()) {
        table->update_row(existing, std::move(*row));
    } else {
        table->add_row(std::move(*row));
    }
    return {};
}

std::optional<SavedConnection> find_connection(const Node& node,
                                               const std::string& name) {
    const std::shared_ptr<data::Table> table = find_store_table(node);
    if (!table) return std::nullopt;
    const zetta::storage::StorageEngine& store = table->store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row) continue;
        const std::optional<SavedConnection> saved = connection_from_row(node, *row);
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
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row) continue;
        std::optional<SavedConnection> saved = connection_from_row(node, *row);
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

ResealReport reseal_connections(Node& node) {
    ResealReport report;
    const std::shared_ptr<data::Table> table = find_store_table(node);
    if (!table) return report;
    const CredentialKeyring* keyring = node.connection_keyring(&report.error);
    if (!keyring) return report;
    const std::string current = keyring->current()->id;
    // Collect first: update_row rewrites the store under the index.
    std::vector<std::pair<std::string, nlohmann::json>> rewrites;
    const zetta::storage::StorageEngine& store = table->store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row || !row->is_object()) continue;
        const auto sealed = row->find("credential");
        if (sealed != row->end() && sealed_key_id(*sealed) == current) continue;
        const std::optional<SavedConnection> saved = connection_from_row(node, *row);
        if (!saved) continue;
        if (saved->credential == CredentialState::Unreadable) {
            report.unreadable.push_back(saved->name);
            continue;
        }
        if (saved->credential == CredentialState::None) continue;
        std::string reason;
        std::optional<nlohmann::json> rewritten =
            stored_row(node, saved->name, saved->config, saved->created, &reason);
        if (!rewritten) {
            report.error = reason;
            continue;
        }
        rewrites.emplace_back(store.id_at(index), std::move(*rewritten));
    }
    for (auto& [id, document] : rewrites) {
        table->update_row(id, std::move(document));
        ++report.resealed;
    }
    // The engine defers file writes to the next tick; a re-seal is settled now,
    // so the plaintext or old-key copy does not outlive a stop before that tick.
    if (!rewrites.empty()) table->store().flush();
    return report;
}

}  // namespace zetta::connect
