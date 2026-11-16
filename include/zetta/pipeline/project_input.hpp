// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::pipeline {

// What a project input represents. The first ingestion slice of FEAT-1 captures
// two kinds (see docs/pipeline/project-inputs.md):
//   - Intent   — natural-language description of what the project should do
//                ("score every active account for churn risk and tell me why").
//   - Artifact — something the team already has (R1: an n8n workflow JSON;
//                SQL / notebooks / schemas planned).
enum class ProjectInputKind { Intent, Artifact };

// How the raw payload arrived — its provenance at the boundary.
//   - Upload — a file the user uploaded (its name is kept in `filename`).
//   - Pasted — text pasted/typed directly (no original filename).
enum class ProjectInputSource { Upload, Pasted };

// Canonical lowercase names — these are BOTH the display label AND the stored
// representation (the strings written to the CRDT value + input.json), so they
// are stable on-disk identifiers, not just labels.
std::string_view to_string(ProjectInputKind kind);
std::string_view to_string(ProjectInputSource source);

// Parse a stored/canonical name back to the enum; std::nullopt if unknown. Used
// for boundary validation when loading an input from disk or the CRDT.
std::optional<ProjectInputKind> project_input_kind_from(std::string_view name);
std::optional<ProjectInputSource> project_input_source_from(std::string_view name);

// A raw input captured into a Project, with provenance — storage + provenance
// only (no parsing/compilation; that happens in later FEAT-1 slices).
//
// A ProjectInput is **replicated state**: the metadata (kind, source, filename,
// content_hash, size, created_by, created_at + the owning project id) is one flat
// key in the Automerge CRDT document (`input:<uuid>`, see src/core/catalog.hpp),
// and a ProjectInput is the in-memory + on-disk projection of it. The raw
// **payload body** is NOT in the document (it would bloat every node's copy);
// like a large object's body it lives on disk and is fingerprinted by
// `content_hash` (a SHA-256 computed at the boundary, never trusted from a
// caller). A Project owns its inputs, exactly as a Database owns its tables;
// lookups hand back shared_ptr handles, never raw pointers.
//
// On disk, each input is a folder named by its UUID under the project's
// `inputs/` directory, holding `input.json` (the metadata) and `payload` (the
// raw bytes):
//
//   data_dir/<project-uuid>/inputs/<input-uuid>/
//     input.json   { id, project, kind, source, filename, content_hash, size,
//                    created_by, created_at }
//     payload      raw bytes (absent on a peer until body transfer is built)
//
// The id is the folder name and never changes; an input is an immutable
// provenance record (created once, never renamed/edited).
class ProjectInput {
public:
    ProjectInput(std::string id, std::string project_id, ProjectInputKind kind,
                 ProjectInputSource source, std::string filename,
                 std::string content_hash, std::uintmax_t size,
                 std::string created_by, std::string created_at,
                 std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& project_id() const { return project_id_; }
    ProjectInputKind kind() const { return kind_; }
    ProjectInputSource source() const { return source_; }
    const std::string& filename() const { return filename_; }  // empty if none
    const std::string& content_hash() const { return content_hash_; }
    std::uintmax_t size() const { return size_; }
    const std::string& created_by() const { return created_by_; }
    const std::string& created_at() const { return created_at_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Where the raw payload body lives on disk (dir/payload).
    std::filesystem::path payload_path() const { return dir_ / "payload"; }
    // Is the payload body present on disk here? (False on a peer that has the
    // metadata but not yet the body — body transfer is a later slice.)
    bool has_body() const;
    // Read the raw payload bytes; nullopt if the body isn't present locally.
    std::optional<std::string> read_payload() const;
    // Write the raw payload body to disk (used on local create). Returns false
    // if the write fails.
    bool write_payload(std::string_view bytes) const;

    // The replicated/on-disk value shape: { project, kind, source, filename,
    // content_hash, size, created_by, created_at }. The id is the CRDT key /
    // folder name, so it isn't repeated here; the payload body is on disk, not
    // here.
    nlohmann::json metadata() const;

    // Write this input's metadata (id + fields) to dir/input.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string project_id_;
    ProjectInputKind kind_;
    ProjectInputSource source_;
    std::string filename_;
    std::string content_hash_;
    std::uintmax_t size_;
    std::string created_by_;
    std::string created_at_;
    std::filesystem::path dir_;
};

}  // namespace zetta::pipeline