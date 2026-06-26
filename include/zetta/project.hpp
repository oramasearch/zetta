#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/project_graph.hpp"
#include "zetta/project_input.hpp"

namespace zetta {

class ReplicationManager;

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
//
// A Project owns its **inputs** (zetta::ProjectInput) the same way a Database
// owns its tables: input create/drop are captured into the replicated CRDT and
// `materialize_input` is the reverse path used by reconciliation. Inputs live in
// the project's `inputs/` subfolder (see project_input.hpp / docs/project-inputs.md).
class Project {
public:
    Project(std::string id, std::string name, std::string created,
            std::string updated, std::string owner, std::filesystem::path dir,
            ReplicationManager* replication = nullptr);

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

    // (Re)load this project's inputs and graphs by scanning dir/inputs and
    // dir/graphs. Called on node start so they survive restart (standalone reads
    // them straight off disk); malformed graph files are skipped (boundary
    // validation).
    void load();

    // --- inputs (this project's raw intent/artifact payloads) --------------

    // Capture a new input with provenance: computes content_hash = sha256(payload)
    // (never trusted from the caller), stamps created_at, writes the metadata into
    // the CRDT (`input:<uuid>`) and the payload body to disk, and returns the
    // handle. `filename` is optional (empty for pasted text); `created_by` is the
    // actor/user key. Returns an empty handle if the input folder can't be made.
    std::shared_ptr<ProjectInput> create_input(ProjectInputKind kind,
                                               ProjectInputSource source,
                                               std::string payload,
                                               std::string filename = {},
                                               std::string created_by = {});
    // Capture a natural-language **intent** string as an input — the FEAT-1
    // "capture NL intent" entry point (#307). A thin, well-named wrapper over
    // create_input that fixes the provenance for typed/pasted intent text
    // (kind=Intent, source=Pasted, no filename) and **rejects empty intent**: an
    // intent that is empty or only whitespace carries no meaning, so it stores
    // nothing (no folder, no CRDT key) and returns an empty handle. `created_by`
    // is the actor/user key. This is storage + provenance only — the text is not
    // parsed or compiled here (that is a later FEAT-1 slice).
    std::shared_ptr<ProjectInput> capture_intent(std::string text,
                                                 std::string created_by = {});
    // Apply a replicated input (from a peer) by id, without capturing. The
    // payload body may not be here yet (body transfer is a later slice). Returns
    // the existing handle if already present.
    std::shared_ptr<ProjectInput> materialize_input(
        std::string id, ProjectInputKind kind, ProjectInputSource source,
        std::string filename, std::string content_hash, std::uintmax_t size,
        std::string created_by, std::string created_at);
    // Find an input by its id (UUID), or an empty handle.
    std::shared_ptr<ProjectInput> find_input(const std::string& id) const;
    // Drop an input (removes its folder + the CRDT key); true if one was removed.
    bool drop_input(const std::string& id);
    // Drop an input by id without capturing (reconciliation removing a peer's
    // drop). Returns whether one was removed.
    bool materialize_drop_input(const std::string& id);

    const std::vector<std::shared_ptr<ProjectInput>>& inputs() const {
        return inputs_;
    }
    std::size_t input_count() const { return inputs_.size(); }

    // --- execution graphs (this project's ExecutionGraph IRs) --------------
    //
    // A project owns zero or more named graphs, persisted under dir/graphs/ and
    // replicated as `graph:<uuid>` keys (see project_graph.hpp). The CRUD mirrors
    // Database's table ownership: create/drop capture into the CRDT, the
    // materialize_* variants are the reverse path used by reconciliation.

    // Store a new named graph. Returns an empty handle if the name is empty or
    // already taken. Assigns a fresh UUID and captures it into the CRDT.
    std::shared_ptr<ProjectGraph> create_graph(std::string name,
                                               ir::ExecutionGraph graph);

    // Replace the contents of an existing graph (found by its current `name`)
    // with `graph`, optionally renaming it to `new_name` (empty = keep the
    // name). Rewrites the on-disk projection AND captures the new value into the
    // CRDT (`graph:<id>`), so an edit replicates exactly like a create — this is
    // the "persist an edit back to the IR" path used by the Graph Workspace.
    // Returns the (same-id) handle, or an empty handle if no graph by `name`
    // exists, `new_name` is empty-after-trim, or `new_name` collides with a
    // different graph in this project.
    std::shared_ptr<ProjectGraph> update_graph(const std::string& name,
                                               ir::ExecutionGraph graph,
                                               std::string new_name = {});

    // Apply a replicated graph (a peer's, or one rebuilt from the document) with
    // a given id, without capturing. Returns the existing handle if present.
    std::shared_ptr<ProjectGraph> materialize_graph(std::string id,
                                                    std::string name,
                                                    ir::ExecutionGraph graph);
    // Drop a graph by id without capturing (reconciliation applying a peer's
    // drop). Returns whether one was removed.
    bool materialize_drop_graph(const std::string& id);

    std::shared_ptr<ProjectGraph> find_graph(const std::string& name) const;
    std::shared_ptr<ProjectGraph> find_graph_by_id(const std::string& id) const;
    bool drop_graph(const std::string& name);

    std::vector<std::string> graph_names() const;
    const std::vector<std::shared_ptr<ProjectGraph>>& graphs() const {
        return graphs_;
    }
    std::size_t graph_count() const { return graphs_.size(); }

private:
    std::filesystem::path inputs_dir() const { return dir_ / "inputs"; }
    std::filesystem::path graph_file(const std::string& graph_id) const;

    std::string id_;
    std::string name_;
    std::string created_;
    std::string updated_;
    std::string owner_;
    std::filesystem::path dir_;
    std::vector<std::shared_ptr<ProjectInput>> inputs_;
    std::vector<std::shared_ptr<ProjectGraph>> graphs_;  // insertion order
    ReplicationManager* replication_;  // null = not replicated
};

}  // namespace zetta
