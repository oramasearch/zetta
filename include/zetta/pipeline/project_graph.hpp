#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"

namespace zetta::pipeline {

// A named ExecutionGraph IR persisted under a Project (see project.hpp). It pairs
// the in-memory graph (zetta::ir::ExecutionGraph) with the identity it needs to
// be stored and replicated: a UUIDv4 id (also its on-disk file name), the id of
// the project it belongs to, and a human name unique within that project.
//
// Like the rest of zetta's state it is **replicated**: the CRDT key
// `graph:<id>` holds { project, name, graph: { nodes, edges } } (the graph
// serialized via zetta::ir::to_json), and the on-disk projection is the same
// record plus its id at <project-uuid>/graphs/<id>.json. The whole graph rides as
// one value (a graph is authored and stored as a unit), so this is the
// coarsest-grained replicated entity — distinct from a row, which owns a key per
// id for conflict-free concurrent edits.
class ProjectGraph {
public:
    ProjectGraph(std::string id, std::string project_id, std::string name,
                 ir::ExecutionGraph graph, std::filesystem::path file);

    const std::string& id() const { return id_; }
    const std::string& project_id() const { return project_id_; }
    const std::string& name() const { return name_; }
    const ir::ExecutionGraph& graph() const { return graph_; }
    const std::filesystem::path& file() const { return file_; }

    // Plan-approval state (#885, the design's review-before-build flow). A
    // compiled Pipeline starts **proposed**; the governed approve_plan capability
    // marks it **approved**; gate/deploy require approval. An edit re-proposes.
    bool approved() const { return approved_; }
    const std::string& approved_by() const { return approved_by_; }
    const std::string& approved_at() const { return approved_at_; }
    // The wire/display spelling the Studio statuses map on: "proposed" / "approved".
    std::string approval_status() const { return approved_ ? "approved" : "proposed"; }

    // Mark the plan approved by a principal at an ISO-8601 instant, then rewrite
    // the on-disk projection. Used by Project::approve_graph (governed, audited).
    void set_approved(std::string approver, std::string at);
    // Restore the replicated approval fields when loading/materializing from the
    // CRDT value or the on-disk record — a pure setter, no persist (the caller
    // owns persistence). Clearing (approved=false) re-proposes the plan.
    void set_approval(bool approved, std::string approver, std::string at);

    // Replace the name + graph to match a (replicated) value, then rewrite the
    // on-disk file. The id, project, file path, and approval are unchanged.
    void set_contents(std::string name, ir::ExecutionGraph graph);

    // Write this graph's record (with its id) to its json file, creating the
    // parent graphs/ folder if needed.
    void persist() const;

    // The replicated / on-disk value shape:
    // { project, name, approval, approved_by, approved_at, graph: {…} }. The id
    // is the CRDT key / file name, so it isn't repeated here (persist() adds it
    // to the on-disk file for self-description, mirroring database.json).
    nlohmann::json record() const;

private:
    std::string id_;
    std::string project_id_;
    std::string name_;
    ir::ExecutionGraph graph_;
    std::filesystem::path file_;
    bool approved_ = false;       // #885: proposed until approve_plan
    std::string approved_by_;     // the approving principal (empty when proposed)
    std::string approved_at_;     // ISO-8601 approval instant (empty when proposed)
};

}  // namespace zetta::pipeline