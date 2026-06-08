#include "zetta/large_object.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"
#include "zetta/uuid.hpp"

#include "catalog.hpp"

namespace zetta {
namespace fs = std::filesystem;
namespace {

std::optional<std::string> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool write_bytes(const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

}  // namespace

LargeObjectStore::LargeObjectStore(fs::path dir, std::string database_id,
                                   ReplicationManager* replication)
    : dir_(std::move(dir)),
      database_id_(std::move(database_id)),
      replication_(replication) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    load();
}

void LargeObjectStore::load() {
    objects_.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "meta.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        if (meta.is_discarded() || !meta.contains("name")) continue;
        LargeObject lo;
        lo.id = entry.path().filename().string();
        lo.name = meta.value("name", std::string{});
        lo.size = meta.value("size", std::uintmax_t{0});
        lo.sha256 = meta.value("sha256", std::string{});
        lo.body_present = fs::exists(entry.path() / "blob");
        objects_.push_back(std::move(lo));
    }
    sort_objects();
}

void LargeObjectStore::write_meta(const LargeObject& object) const {
    const fs::path obj = object_dir(object.id);
    std::error_code ec;
    fs::create_directories(obj, ec);
    const nlohmann::json meta{{"id", object.id},
                              {"name", object.name},
                              {"size", object.size},
                              {"sha256", object.sha256}};
    std::ofstream m(obj / "meta.json", std::ios::trunc);
    m << meta.dump(2) << '\n';
}

void LargeObjectStore::sort_objects() {
    std::sort(objects_.begin(), objects_.end(),
              [](const LargeObject& a, const LargeObject& b) {
                  return a.name < b.name;
              });
}

std::optional<LargeObject> LargeObjectStore::import_file(const fs::path& src) {
    const auto bytes = read_bytes(src);
    if (!bytes) return std::nullopt;

    LargeObject lo;
    lo.id = new_uuid();
    lo.name = src.filename().string();
    lo.size = bytes->size();
    lo.sha256 = sha256_hex(*bytes);
    lo.body_present = true;

    const fs::path obj = object_dir(lo.id);
    std::error_code ec;
    fs::create_directories(obj, ec);
    if (ec) return std::nullopt;
    if (!write_bytes(obj / "blob", *bytes)) return std::nullopt;
    write_meta(lo);

    // Capture the metadata into the CRDT (the body transfers separately).
    if (replication_) {
        replication_->set(catalog::large_object_key(lo.id),
                          nlohmann::json{{"db", database_id_},
                                         {"name", lo.name},
                                         {"size", lo.size},
                                         {"sha256", lo.sha256}});
    }

    objects_.push_back(lo);
    sort_objects();
    return lo;
}

LargeObject LargeObjectStore::materialize_object(LargeObject object) {
    // Already known: just refresh whether the body has landed since (the
    // transfer runs independently of metadata reconciliation).
    for (LargeObject& existing : objects_) {
        if (existing.id == object.id) {
            existing.body_present = has_body(existing.id);
            return existing;
        }
    }
    object.body_present = has_body(object.id);
    write_meta(object);  // metadata only; the body arrives via transfer
    objects_.push_back(object);
    sort_objects();
    return object;
}

bool LargeObjectStore::materialize_unlink(const std::string& id) {
    const auto it = std::find_if(
        objects_.begin(), objects_.end(),
        [&](const LargeObject& lo) { return lo.id == id; });
    if (it == objects_.end()) return false;
    std::error_code ec;
    fs::remove_all(object_dir(id), ec);
    objects_.erase(it);
    return true;
}

bool LargeObjectStore::export_file(const std::string& id,
                                   const fs::path& dst) const {
    const auto bytes = read(id);
    if (!bytes) return false;
    return write_bytes(dst, *bytes);
}

std::optional<std::string> LargeObjectStore::read(const std::string& id) const {
    if (!find(id)) return std::nullopt;
    return read_bytes(object_dir(id) / "blob");
}

bool LargeObjectStore::has_body(const std::string& id) const {
    std::error_code ec;
    return fs::exists(object_dir(id) / "blob", ec);
}

bool LargeObjectStore::unlink(const std::string& id) {
    if (!materialize_unlink(id)) return false;
    if (replication_) replication_->remove(catalog::large_object_key(id));
    return true;
}

std::vector<LargeObject> LargeObjectStore::list() const {
    // Refresh body_present from disk each call: a body may have arrived via
    // peer transfer since the metadata was registered, without a reconcile.
    std::vector<LargeObject> result = objects_;
    for (LargeObject& lo : result) lo.body_present = has_body(lo.id);
    return result;
}

const LargeObject* LargeObjectStore::find(const std::string& id) const {
    for (const LargeObject& lo : objects_) {
        if (lo.id == id) return &lo;
    }
    return nullptr;
}

fs::path LargeObjectStore::object_dir(const std::string& id) const {
    return dir_ / id;
}

}  // namespace zetta
