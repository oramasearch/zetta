// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::workspace {

// The replicated/on-disk value of a Workspace Profile — everything the CRDT key
// `profile:<uuid>` carries (the id is the key / folder name, so it isn't repeated
// here). This is exactly the shape validated by the Workspace Profile JSON Schema
// at every boundary.
//
// A Workspace Profile is the input to the "Discover Opportunities" pathway
// (#886): the owning user's workspace **role** and a plain-language
// **daily_work** description, plus the **sources** they have approved for the
// environment scan (refs to datasets/connections, each a free-form
// `{ id, name, kind }`). The profile is real replicated state immediately; the
// Auto-Suggest model (#554) consumes it to rank Opportunities when it ships,
// and until then the Opportunities surface stays honestly in-training.
//
// `owner` is the principal id the profile belongs to (required, non-empty).
// `created` / `updated` are ISO-8601 UTC stamps.
struct WorkspaceProfileValue {
    std::string owner;       // the owning principal id (required, non-empty)
    std::string role;        // the user's workspace role (e.g. "Customer success lead")
    std::string daily_work;  // plain-language description of their daily work
    nlohmann::json sources = nlohmann::json::array();  // approved source refs [{id,name,kind}]
    std::string created;     // ISO-8601 UTC stamp at creation
    std::string updated;     // ISO-8601 UTC stamp at the last edit
};

// Serialize a profile value to its JSON shape (no id). The inverse of
// parse_workspace_profile_value; used for the CRDT value, workspace_profile.json,
// and tests.
nlohmann::json to_json(const WorkspaceProfileValue& value);

// The Workspace Profile JSON Schema (draft-07) — the single source of truth for
// the value shape, exposed so callers/tests can introspect it.
const nlohmann::json& workspace_profile_value_schema();

// Boundary validation: check a JSON value against the schema. Returns true when
// valid; on failure returns false and (if `error` is non-null) fills it with a
// human-readable reason.
bool validate_workspace_profile_value(const nlohmann::json& value,
                                      std::string* error = nullptr);

// Parse + validate a JSON value (from workspace_profile.json on disk or a
// `profile:<id>` CRDT value) into a WorkspaceProfileValue. Returns nullopt when
// the value fails schema validation, so malformed data is rejected at the
// boundary with a clear reason (in `error`) instead of corrupting the tree.
std::optional<WorkspaceProfileValue> parse_workspace_profile_value(
    const nlohmann::json& value, std::string* error = nullptr);

// A Workspace Profile record: a top-level replicated entity with no parent,
// modeled like zetta::ml::Model — **replicated state** (flat key
// `profile:<uuid>`) whose in-memory + on-disk
// (`<profile-uuid>/workspace_profile.json`) forms are a projection of the
// Automerge CRDT document. The id is the folder name and never changes; `owner`
// and `created` are fixed at creation. set_content captures into the CRDT (when
// replicated) and rewrites the file; reconciliation uses apply_value() (no
// capture). Lookups hand back shared_ptr handles. See docs/workspace/.
class WorkspaceProfile {
public:
    WorkspaceProfile(std::string id, WorkspaceProfileValue value,
                     std::filesystem::path dir,
                     zetta::cluster::ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& owner() const { return value_.owner; }
    const std::string& role() const { return value_.role; }
    const std::string& daily_work() const { return value_.daily_work; }
    const nlohmann::json& sources() const { return value_.sources; }
    const std::string& created() const { return value_.created; }
    const std::string& updated() const { return value_.updated; }
    const WorkspaceProfileValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Update the editable fields (role, daily-work, approved sources), stamp
    // `updated`, then capture + persist. The owner + created stay fixed.
    void set_content(std::string role, std::string daily_work,
                     nlohmann::json sources, std::string updated);

    // Replace the value to match a (replicated) document value, then rewrite the
    // file — with NO capture (used by reconciliation).
    void apply_value(WorkspaceProfileValue value);

    // Write this profile's value (+ id) to dir/workspace_profile.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    void capture_and_persist();  // capture (if replicated), rewrite the file

    std::string id_;
    WorkspaceProfileValue value_;
    std::filesystem::path dir_;
    zetta::cluster::ReplicationManager* replication_;  // null = standalone
};

}  // namespace zetta::workspace
