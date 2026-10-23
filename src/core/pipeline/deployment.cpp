#include "zetta/pipeline/deployment.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta::pipeline {
namespace fs = std::filesystem;

Deployment::Deployment(std::string id, std::string graph_name,
                       DeploymentTarget target, std::string node_id,
                       std::size_t node_count, std::size_t edge_count,
                       std::string created, fs::path dir,
                       std::vector<TableRead> reads)
    : id_(std::move(id)),
      graph_name_(std::move(graph_name)),
      target_(std::move(target)),
      node_id_(std::move(node_id)),
      node_count_(node_count),
      edge_count_(edge_count),
      created_(std::move(created)),
      dir_(std::move(dir)),
      reads_(std::move(reads)) {}

nlohmann::json Deployment::metadata() const {
    nlohmann::json meta{{"graph_name", graph_name_},
                        {"target", target_.to_json()},
                        {"node", node_id_},
                        {"node_count", node_count_},
                        {"edge_count", edge_count_},
                        {"created", created_}};
    // Only when there is one, so a deployment that reads nothing keeps the
    // shape an older peer wrote.
    if (!reads_.empty()) meta["reads"] = to_json(reads_);
    return meta;
}

void Deployment::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "deployment.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::pipeline