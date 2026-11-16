// SPDX-License-Identifier: MIT

#include "zetta/pipeline/project_graph.hpp"

#include <fstream>
#include <type_traits>
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
    nlohmann::json record{{"project", project_id_},
                          {"name", name_},
                          {"approval", approval_status()},
                          {"approved_by", approved_by_},
                          {"approved_at", approved_at_},
                          {"graph", ir::to_json(graph_)}};
    if (exposure_.version > 0) record["exposure"] = to_json(exposure_);
    return record;
}

nlohmann::json to_json(const PipelineExposure& exposure) {
    return nlohmann::json{{"exposed", exposure.exposed},
                          {"version", exposure.version},
                          {"pinned", exposure.pinned},
                          {"input_schema", exposure.input_schema},
                          {"rate_limit_per_minute", exposure.rate_limit_per_minute},
                          {"max_input_bytes", exposure.max_input_bytes},
                          {"sync_timeout_ms", exposure.sync_timeout_ms},
                          {"exposed_by", exposure.exposed_by},
                          {"exposed_at", exposure.exposed_at}};
}

std::optional<PipelineExposure> pipeline_exposure_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto boolean = value.find("exposed");
    const auto version = value.find("version");
    const auto pinned = value.find("pinned");
    const auto schema = value.find("input_schema");
    if (boolean == value.end() || !boolean->is_boolean() || version == value.end() ||
        !version->is_number_unsigned() || pinned == value.end() ||
        !pinned->is_object() || schema == value.end() || !schema->is_object()) {
        return std::nullopt;
    }
    // The pinned graph must parse against the IR schema, or nothing is served.
    if (!ir::from_json(*pinned)) return std::nullopt;
    PipelineExposure exposure;
    exposure.exposed = boolean->get<bool>();
    exposure.version = version->get<std::uint64_t>();
    exposure.pinned = *pinned;
    exposure.input_schema = *schema;
    const auto unsigned_field = [&](const char* key, auto& out) {
        const auto found = value.find(key);
        if (found == value.end()) return true;
        if (!found->is_number_unsigned()) return false;
        out = found->get<std::remove_reference_t<decltype(out)>>();
        return true;
    };
    if (!unsigned_field("rate_limit_per_minute", exposure.rate_limit_per_minute) ||
        !unsigned_field("max_input_bytes", exposure.max_input_bytes) ||
        !unsigned_field("sync_timeout_ms", exposure.sync_timeout_ms)) {
        return std::nullopt;
    }
    const auto text = [&](const char* key, std::string& out) {
        const auto found = value.find(key);
        if (found == value.end()) return true;
        if (!found->is_string()) return false;
        out = found->get<std::string>();
        return true;
    };
    if (!text("exposed_by", exposure.exposed_by) || !text("exposed_at", exposure.exposed_at)) {
        return std::nullopt;
    }
    return exposure;
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