#include "zetta/project_input.hpp"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

std::string_view to_string(ProjectInputKind kind) {
    switch (kind) {
        case ProjectInputKind::Intent:   return "intent";
        case ProjectInputKind::Artifact: return "artifact";
    }
    return "unknown";
}

std::string_view to_string(ProjectInputSource source) {
    switch (source) {
        case ProjectInputSource::Upload: return "upload";
        case ProjectInputSource::Pasted: return "pasted";
    }
    return "unknown";
}

std::optional<ProjectInputKind> project_input_kind_from(std::string_view name) {
    if (name == "intent") return ProjectInputKind::Intent;
    if (name == "artifact") return ProjectInputKind::Artifact;
    return std::nullopt;
}

std::optional<ProjectInputSource> project_input_source_from(
    std::string_view name) {
    if (name == "upload") return ProjectInputSource::Upload;
    if (name == "pasted") return ProjectInputSource::Pasted;
    return std::nullopt;
}

ProjectInput::ProjectInput(std::string id, std::string project_id,
                           ProjectInputKind kind, ProjectInputSource source,
                           std::string filename, std::string content_hash,
                           std::uintmax_t size, std::string created_by,
                           std::string created_at, fs::path dir)
    : id_(std::move(id)),
      project_id_(std::move(project_id)),
      kind_(kind),
      source_(source),
      filename_(std::move(filename)),
      content_hash_(std::move(content_hash)),
      size_(size),
      created_by_(std::move(created_by)),
      created_at_(std::move(created_at)),
      dir_(std::move(dir)) {}

bool ProjectInput::has_body() const {
    std::error_code ec;
    return fs::exists(payload_path(), ec);
}

std::optional<std::string> ProjectInput::read_payload() const {
    std::ifstream in(payload_path(), std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool ProjectInput::write_payload(std::string_view bytes) const {
    std::ofstream out(payload_path(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

nlohmann::json ProjectInput::metadata() const {
    return nlohmann::json{{"project", project_id_},
                          {"kind", to_string(kind_)},
                          {"source", to_string(source_)},
                          {"filename", filename_},
                          {"content_hash", content_hash_},
                          {"size", size_},
                          {"created_by", created_by_},
                          {"created_at", created_at_}};
}

void ProjectInput::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "input.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta
