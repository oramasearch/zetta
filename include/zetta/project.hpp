#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace zetta {

// An AI project — the central, user-managed artifact in Orama Zetta (a project
// is instantiated from a use-case type on a team's data; see docs/projects.md).
// This is the data-model root only: id + name + lifecycle stamps + owner key.
// Its inputs / execution graphs / runs attach in later slices (FEAT-1/FEAT-2).
//
// A Project is **replicated state**: the Automerge CRDT document is the source
// of truth (flat key `project:<uuid>`), and a Project is the in-memory + on-disk
// projection of it — persisted in its own folder (named by the UUID) under the
// node's data dir, with project.json holding the metadata. Lookups hand back
// shared_ptr handles, never raw pointers, exactly like Database.
//
// The id is the folder name and never changes; rename rewrites project.json and
// bumps `updated`, but keeps the folder/id. `metadata()` is the value shape that
// rides in the CRDT and (with the id added) on disk.
class Project {
public:
    Project(std::string id, std::string name, std::string created,
            std::string updated, std::string owner,
            std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& created() const { return created_; }
    const std::string& updated() const { return updated_; }
    const std::string& owner() const { return owner_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Replace the mutable metadata to match a (replicated) value, then rewrite
    // project.json. The folder/id is unchanged. `created` is logically
    // immutable but accepted so the projection matches the document exactly.
    void set_metadata(std::string name, std::string created,
                      std::string updated, std::string owner);

    // Write this project's metadata (id + fields) to dir/project.json.
    void persist_meta() const;

    // The replicated/on-disk value shape: { name, created, updated, owner }.
    // (The id is the CRDT key / folder name, so it isn't repeated here.)
    nlohmann::json metadata() const;

private:
    std::string id_;
    std::string name_;
    std::string created_;
    std::string updated_;
    std::string owner_;
    std::filesystem::path dir_;
};

}  // namespace zetta
