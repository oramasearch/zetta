#include "zetta/node.hpp"

#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"
#include "zetta/uuid.hpp"
#include "zetta/version.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

Node::Node(NodeConfig config, ReplicationManager* replication)
    : config_(std::move(config)), replication_(replication) {}

bool Node::start() {
    if (running_) return false;
    running_ = true;
    started_at_ = Clock::now();
    ticks_ = 0;
    load_databases();
    if (replication_) {
        load_replication();         // bring in the persisted CRDT document
        migrate_to_replication();   // capture any local-only data into it
        reconcile_from_replication();  // materialize anything only in the document
        persist_replication();
        last_seen_version_ = replication_->version();
    }
    std::printf("zetta %s: node started (data_dir=%s, %zu databases)\n",
                kVersion, config_.data_dir.c_str(), databases_.size());
    if (config_.preflight) {
        integrity_ = check_integrity();
        std::printf(
            "zetta: preflight: %zu databases, %zu tables, %zu rows, %zu large "
            "objects (%zu present, %zu awaiting body) — %s\n",
            integrity_.databases, integrity_.tables, integrity_.rows,
            integrity_.large_objects, integrity_.large_object_bodies_present,
            integrity_.large_object_bodies_missing,
            integrity_.ok() ? "OK" : "ISSUES FOUND");
        if (!integrity_.ok()) {
            std::printf(
                "zetta: preflight WARN: %zu corrupt large object(s), %zu "
                "dangling table(s), %zu dangling row(s), %zu dangling large "
                "object(s)\n",
                integrity_.corrupt_large_objects, integrity_.dangling_tables,
                integrity_.dangling_rows, integrity_.dangling_large_objects);
        }
    }
    std::fflush(stdout);
    return true;
}

void Node::tick() {
    if (!running_) return;
    ++ticks_;
    // The CRDT is the source of truth; when it has advanced (a local capture or
    // a peer's gossip merged in on the network thread), reconcile the on-disk
    // projection to match it and persist. Reconcile only materializes (never
    // captures), so it doesn't move the version — no feedback loop.
    if (replication_) {
        std::string version = replication_->version();
        if (version != last_seen_version_) {
            reconcile_from_replication();
            persist_replication();
            last_seen_version_ = std::move(version);
        }
    }
}

void Node::stop() {
    if (!running_) return;
    if (replication_) persist_replication();
    running_ = false;
    std::printf("zetta: node stopped\n");
    std::fflush(stdout);
}

double Node::uptime_seconds() const {
    if (!running_) return 0.0;
    const std::chrono::duration<double> elapsed = Clock::now() - started_at_;
    return elapsed.count();
}

void Node::load_databases() {
    databases_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "database.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        if (meta.is_discarded() || !meta.contains("name")) continue;
        auto db = std::make_shared<Database>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(), entry.path(), replication_);
        db->load();
        databases_.push_back(std::move(db));
    }
    // Stable order for the UI (directory iteration order is unspecified).
    std::sort(databases_.begin(), databases_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<Database> Node::create_database(std::string name) {
    // "system" is reserved for the virtual system database (computed system
    // tables like system.zetta_nodes); never a real on-disk database.
    if (name.empty() || name == "system" || find_database(name)) return nullptr;
    const std::string id = new_uuid();
    if (replication_) {
        replication_->set(catalog::database_key(id),
                          nlohmann::json{{"name", name}});
    }
    return materialize_database(id, std::move(name));
}

std::shared_ptr<Database> Node::materialize_database(std::string id,
                                                     std::string name) {
    if (auto existing = find_database_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto db = std::make_shared<Database>(std::move(id), std::move(name), dir,
                                         replication_);
    db->persist_meta();
    databases_.push_back(db);
    return db;
}

std::shared_ptr<Database> Node::find_database(const std::string& name) const {
    for (const auto& db : databases_) {
        if (db->name() == name) return db;
    }
    return nullptr;
}

std::shared_ptr<Database> Node::find_database_by_id(
    const std::string& id) const {
    for (const auto& db : databases_) {
        if (db->id() == id) return db;
    }
    return nullptr;
}

bool Node::drop_database(const std::string& name) {
    auto db = find_database(name);
    if (!db) return false;
    if (replication_) replication_->remove(catalog::database_key(db->id()));
    return materialize_drop_database(db->id());
}

bool Node::materialize_drop_database(const std::string& id) {
    const auto it = std::find_if(
        databases_.begin(), databases_.end(),
        [&](const std::shared_ptr<Database>& d) { return d->id() == id; });
    if (it == databases_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    databases_.erase(it);
    return true;
}

bool Node::rename_database(const std::string& from, const std::string& to) {
    if (to.empty() || find_database(to)) return false;  // empty or clashes
    auto db = find_database(from);
    if (!db) return false;
    if (replication_) {
        replication_->set(catalog::database_key(db->id()),
                          nlohmann::json{{"name", to}});
    }
    db->set_name(to);
    return true;
}

std::vector<std::string> Node::database_names() const {
    std::vector<std::string> names;
    names.reserve(databases_.size());
    for (const auto& db : databases_) names.push_back(db->name());
    return names;
}

// ----- replication: persistence ---------------------------------------------

fs::path Node::replication_path() const {
    return fs::path(config_.data_dir) / "replication.automerge";
}

void Node::load_replication() {
    std::error_code ec;
    const fs::path path = replication_path();
    if (!fs::exists(path, ec)) return;
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    if (raw.empty()) return;
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    replication_->merge(bytes);
}

void Node::persist_replication() const {
    const std::vector<std::byte> bytes = replication_->save();
    const fs::path path = replication_path();
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);  // atomic replace
}

// ----- replication: capture local-only data ---------------------------------

void Node::migrate_to_replication() {
    const nlohmann::json snapshot = replication_->snapshot();
    for (const auto& db : databases_) {
        if (!snapshot.contains(catalog::database_key(db->id()))) {
            replication_->set(catalog::database_key(db->id()),
                              nlohmann::json{{"name", db->name()}});
        }
        for (const auto& table : db->tables()) {
            if (!snapshot.contains(catalog::table_key(table->id()))) {
                replication_->set(
                    catalog::table_key(table->id()),
                    nlohmann::json{{"db", db->id()}, {"name", table->name()}});
            }
            const StorageEngine& store = table->store();
            for (std::size_t i = 0; i < store.size(); ++i) {
                const std::string& row_id = store.id_at(i);
                if (!snapshot.contains(catalog::row_key(row_id))) {
                    replication_->set(
                        catalog::row_key(row_id),
                        nlohmann::json{{"tbl", table->id()},
                                       {"data", store.at(i)}});
                }
            }
        }
        for (const LargeObject& lo : db->large_objects().list()) {
            if (!snapshot.contains(catalog::large_object_key(lo.id))) {
                replication_->set(catalog::large_object_key(lo.id),
                                  nlohmann::json{{"db", db->id()},
                                                 {"name", lo.name},
                                                 {"size", lo.size},
                                                 {"sha256", lo.sha256}});
            }
        }
    }
}

// ----- replication: apply the document to the local projection --------------

void Node::reconcile_from_replication() {
    const nlohmann::json snapshot = replication_->snapshot();

    // Decode the flat catalog keys into desired databases / tables / rows.
    std::map<std::string, std::string> databases;             // id -> name
    std::map<std::string, std::pair<std::string, std::string>> tables;  // id -> (db, name)
    struct RowEntry { std::string table_id; nlohmann::json data; };
    std::map<std::string, RowEntry> rows;                     // id -> entry
    struct LargeObjectEntry {
        std::string database_id;
        std::string name;
        std::uintmax_t size;
        std::string sha256;
    };
    std::map<std::string, LargeObjectEntry> large_objects;    // id -> entry

    for (const auto& [key, value] : snapshot.items()) {
        if (std::string id = catalog::id_after(key, catalog::kDatabasePrefix);
            !id.empty() && value.is_object() && value.contains("name")) {
            databases.emplace(std::move(id), value["name"].get<std::string>());
        } else if (std::string table_id =
                       catalog::id_after(key, catalog::kTablePrefix);
                   !table_id.empty() && value.is_object() &&
                   value.contains("db") && value.contains("name")) {
            tables.emplace(std::move(table_id),
                           std::pair{value["db"].get<std::string>(),
                                     value["name"].get<std::string>()});
        } else if (std::string row_id =
                       catalog::id_after(key, catalog::kRowPrefix);
                   !row_id.empty() && value.is_object() &&
                   value.contains("tbl") && value.contains("data")) {
            rows.emplace(std::move(row_id),
                         RowEntry{value["tbl"].get<std::string>(),
                                  value["data"]});
        } else if (std::string lo_id =
                       catalog::id_after(key, catalog::kLargeObjectPrefix);
                   !lo_id.empty() && value.is_object() && value.contains("db")) {
            large_objects.emplace(
                std::move(lo_id),
                LargeObjectEntry{value.value("db", std::string{}),
                                 value.value("name", std::string{}),
                                 value.value("size", std::uintmax_t{0}),
                                 value.value("sha256", std::string{})});
        }
    }

    // Removals first (a peer dropped a database/table). Snapshot ids up front so
    // we don't mutate while iterating.
    std::vector<std::string> local_db_ids;
    for (const auto& db : databases_) local_db_ids.push_back(db->id());
    for (const std::string& db_id : local_db_ids) {
        if (!databases.contains(db_id)) {
            materialize_drop_database(db_id);
            continue;
        }
        auto db = find_database_by_id(db_id);
        std::vector<std::string> local_table_ids;
        for (const auto& table : db->tables()) {
            local_table_ids.push_back(table->id());
        }
        for (const std::string& table_id : local_table_ids) {
            const auto it = tables.find(table_id);
            if (it == tables.end() || it->second.first != db_id) {
                db->materialize_drop_table(table_id);
            }
        }
        std::vector<std::string> local_lo_ids;
        for (const LargeObject& lo : db->large_objects().list()) {
            local_lo_ids.push_back(lo.id);
        }
        for (const std::string& lo_id : local_lo_ids) {
            const auto it = large_objects.find(lo_id);
            if (it == large_objects.end() || it->second.database_id != db_id) {
                db->large_objects().materialize_unlink(lo_id);
            }
        }
    }

    // Additions / renames.
    for (const auto& [id, name] : databases) {
        auto db = find_database_by_id(id);
        if (!db) {
            materialize_database(id, name);
        } else if (db->name() != name) {
            db->set_name(name);  // a peer renamed it (no re-capture)
        }
    }
    for (const auto& [id, db_and_name] : tables) {
        auto db = find_database_by_id(db_and_name.first);
        if (db && !db->find_table_by_id(id)) {
            db->materialize_table(id, db_and_name.second);
        }
    }
    for (auto& [id, entry] : rows) {
        auto db_table = [&]() -> std::shared_ptr<Table> {
            for (const auto& db : databases_) {
                if (auto table = db->find_table_by_id(entry.table_id)) {
                    return table;
                }
            }
            return nullptr;
        }();
        if (db_table) db_table->materialize_row(id, std::move(entry.data));
    }
    for (const auto& [id, entry] : large_objects) {
        auto db = find_database_by_id(entry.database_id);
        if (!db) continue;
        LargeObject object;
        object.id = id;
        object.name = entry.name;
        object.size = entry.size;
        object.sha256 = entry.sha256;
        db->large_objects().materialize_object(std::move(object));
    }
}

IntegrityReport Node::check_integrity() const {
    IntegrityReport report;
    for (const auto& db : databases_) {
        ++report.databases;
        for (const auto& table : db->tables()) {
            ++report.tables;
            report.rows += table->row_count();
        }
        for (const LargeObject& object : db->large_objects().list()) {
            ++report.large_objects;
            if (!object.body_present) {
                ++report.large_object_bodies_missing;
                continue;
            }
            ++report.large_object_bodies_present;
            // Re-hash the on-disk body and compare to the recorded digest.
            const std::optional<std::string> body =
                db->large_objects().read(object.id);
            if (!body || (!object.sha256.empty() &&
                          sha256_hex(*body) != object.sha256)) {
                ++report.corrupt_large_objects;
            }
        }
    }

    // Dangling references in the replicated catalog: a table/large object whose
    // database is gone, or a row whose table is gone (orphaned keys the
    // reconciler ignores but that shouldn't accumulate).
    if (replication_) {
        const nlohmann::json snapshot = replication_->snapshot();
        std::unordered_set<std::string> database_ids;
        std::unordered_set<std::string> table_ids;
        for (const auto& [key, value] : snapshot.items()) {
            if (std::string id = catalog::id_after(key, catalog::kDatabasePrefix);
                !id.empty()) {
                database_ids.insert(std::move(id));
            } else if (std::string table_id =
                           catalog::id_after(key, catalog::kTablePrefix);
                       !table_id.empty()) {
                table_ids.insert(std::move(table_id));
            }
        }
        const auto parent_missing = [](const nlohmann::json& value,
                                       const char* field,
                                       const std::unordered_set<std::string>& set) {
            return value.is_object() && value.contains(field) &&
                   !set.contains(value[field].get<std::string>());
        };
        for (const auto& [key, value] : snapshot.items()) {
            if (!catalog::id_after(key, catalog::kTablePrefix).empty()) {
                if (parent_missing(value, "db", database_ids)) ++report.dangling_tables;
            } else if (!catalog::id_after(key, catalog::kRowPrefix).empty()) {
                if (parent_missing(value, "tbl", table_ids)) ++report.dangling_rows;
            } else if (!catalog::id_after(key, catalog::kLargeObjectPrefix).empty()) {
                if (parent_missing(value, "db", database_ids)) {
                    ++report.dangling_large_objects;
                }
            }
        }
    }
    return report;
}

std::string Node::status_line() const {
    if (!running_) return "stopped";
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "running — up %.0fs, %llu ticks, %zu databases",
                  uptime_seconds(),
                  static_cast<unsigned long long>(ticks_),
                  databases_.size());
    return buf;
}

}  // namespace zetta
