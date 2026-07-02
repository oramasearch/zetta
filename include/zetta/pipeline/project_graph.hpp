#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"

namespace zetta {

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

    // Replace the name + graph to match a (replicated) value, then rewrite the
    // on-disk file. The id, project, and file path are unchanged.
    void set_contents(std::string name, ir::ExecutionGraph graph);

    // Write this graph's record (with its id) to its json file, creating the
    // parent graphs/ folder if needed.
    void persist() const;

    // The replicated / on-disk value shape: { project, name, graph: {…} }. The
    // id is the CRDT key / file name, so it isn't repeated here (persist() adds
    // it to the on-disk file for self-description, mirroring database.json).
    nlohmann::json record() const;

private:
    std::string id_;
    std::string project_id_;
    std::string name_;
    ir::ExecutionGraph graph_;
    std::filesystem::path file_;
};

}  // namespace zetta
