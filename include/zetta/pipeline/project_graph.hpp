#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"

namespace zetta::pipeline {

// A published pipeline's exposure (zetta#1289, docs/api/serving.md): the
// replicated state that decides whether `POST /v1/pipelines/{name}/run` answers
// for this pipeline, on every node. Exposing **pins** the pipeline: `pinned` is
// the graph as it was when it was exposed, and `version` counts the exposures,
// so an edit to the stored graph changes nothing callers get until the pipeline
// is exposed again. `input_schema` is what the exposer declared callers may
// pass (a draft-07 object schema over `run`'s inputs; the default admits none).
// The limits are per caller and per node. Unexposing keeps the record with
// `exposed` false, so the next exposure continues the version count.
struct PipelineExposure {
    bool exposed = false;
    std::uint64_t version = 0;            // 0 = never exposed
    nlohmann::json pinned;                // ir::to_json of the graph when exposed
    nlohmann::json input_schema;          // draft-07, type object
    std::uint32_t rate_limit_per_minute = 60;
    std::uint64_t max_input_bytes = 65536;
    std::uint32_t sync_timeout_ms = 30000;
    std::string exposed_by;               // the principal that exposed it
    std::string exposed_at;               // ISO-8601
    bool operator==(const PipelineExposure&) const = default;
};

// The exposure's replicated/on-disk shape (every field above).
nlohmann::json to_json(const PipelineExposure& exposure);
// Parse it at the boundary (a peer's value, a file): nullopt when the shape is
// wrong, so a malformed value is never trusted into serving.
std::optional<PipelineExposure> pipeline_exposure_from_json(const nlohmann::json& value);

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
    // Publication (zetta#1289): whether the pipeline is exposed as a served
    // route, and the pinned version callers get. Pure setter like
    // set_approval; Project::set_graph_exposure persists and captures it.
    const PipelineExposure& exposure() const { return exposure_; }
    void set_exposure(PipelineExposure exposure) { exposure_ = std::move(exposure); }

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
    // { project, name, approval, approved_by, approved_at, graph: {…},
    //   exposure? } (`exposure` only once the pipeline has been exposed, so a
    //   never-published graph keeps its pre-#1289 shape). The id
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
    PipelineExposure exposure_;   // zetta#1289: never exposed by default
};

}  // namespace zetta::pipeline