#include "zetta/pipeline/project_graph.hpp"

#include <fstream>
#include <system_error>
#include <utility>

#include "zetta/ir/serialization.hpp"

namespace zetta::pipeline {
namespace fs = std::filesystem;

ProjectGraph::ProjectGraph(std::string id, std::string project_id,
                           std::string name, ir::ExecutionGraph graph,
                           fs::path file)
    : id_(std::move(id)),
      project_id_(std::move(project_id)),
      name_(std::move(name)),
      graph_(std::move(graph)),
      file_(std::move(file)) {}

void ProjectGraph::set_contents(std::string name, ir::ExecutionGraph graph) {
    name_ = std::move(name);
    graph_ = std::move(graph);
    persist();
}

void ProjectGraph::set_approved(std::string approver, std::string at) {
    approved_ = true;
    approved_by_ = std::move(approver);
    approved_at_ = std::move(at);
    persist();
}

void ProjectGraph::set_approval(bool approved, std::string approver,
                                std::string at) {
    approved_ = approved;
    approved_by_ = std::move(approver);
    approved_at_ = std::move(at);
}

nlohmann::json ProjectGraph::record() const {
    return nlohmann::json{{"project", project_id_},
                          {"name", name_},
                          {"approval", approval_status()},
                          {"approved_by", approved_by_},
                          {"approved_at", approved_at_},
                          {"graph", ir::to_json(graph_)}};
}

void ProjectGraph::persist() const {
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    nlohmann::json document = record();
    document["id"] = id_;
    std::ofstream out(file_, std::ios::trunc);
    out << document.dump(2) << '\n';
}

}  // namespace zetta::pipeline