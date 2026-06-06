#include "zetta/large_object.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/uuid.hpp"

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

LargeObjectStore::LargeObjectStore(fs::path dir) : dir_(std::move(dir)) {
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
        objects_.push_back(std::move(lo));
    }
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

    const fs::path obj = object_dir(lo.id);
    std::error_code ec;
    fs::create_directories(obj, ec);
    if (ec) return std::nullopt;
    if (!write_bytes(obj / "blob", *bytes)) return std::nullopt;

    const nlohmann::json meta{{"id", lo.id}, {"name", lo.name},
                              {"size", lo.size}};
    std::ofstream m(obj / "meta.json", std::ios::trunc);
    m << meta.dump(2) << '\n';

    objects_.push_back(lo);
    std::sort(objects_.begin(), objects_.end(),
              [](const LargeObject& a, const LargeObject& b) {
                  return a.name < b.name;
              });
    return lo;
}

bool LargeObjectStore::export_file(const std::string& id,
                                   const fs::path& dst) const {
    if (!find(id)) return false;
    const auto bytes = read_bytes(object_dir(id) / "blob");
    if (!bytes) return false;
    return write_bytes(dst, *bytes);
}

std::optional<std::string> LargeObjectStore::read(const std::string& id) const {
    if (!find(id)) return std::nullopt;
    return read_bytes(object_dir(id) / "blob");
}

bool LargeObjectStore::unlink(const std::string& id) {
    const auto it = std::find_if(
        objects_.begin(), objects_.end(),
        [&](const LargeObject& lo) { return lo.id == id; });
    if (it == objects_.end()) return false;
    std::error_code ec;
    fs::remove_all(object_dir(id), ec);
    objects_.erase(it);
    return true;
}

std::vector<LargeObject> LargeObjectStore::list() const { return objects_; }

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
