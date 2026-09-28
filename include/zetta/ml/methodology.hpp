#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"
#include "zetta/ir/execution_graph.hpp"

namespace zetta::pipeline {
class Project;
class ProjectGraph;
} // namespace zetta::pipeline

// The methodology registry (#1061): one table describes every methodology the
// engine recognizes and supplies the callbacks for methodologies it can run.
// Compilers and run dispatch use this interface, so adding a methodology does
// not add another branch to either caller.
//
// See docs/ml/methodologies.md.
namespace zetta::ml {

struct MethodologyInput {
    std::string role;
    bool required = false;
    std::string description;
};

struct ShapeRequest {
    std::string project;
    std::string intent;
    std::string solution;
    nlohmann::json hints;
};

struct RunRequest {
    zetta::api::ApiContext* context = nullptr;
    const pipeline::Project* project = nullptr;
    const pipeline::ProjectGraph* graph = nullptr;
    nlohmann::json input;
};

struct Methodology {
    std::string id;
    std::string name;
    std::string family;
    std::vector<std::string> model_kinds;
    std::vector<std::string> solutions;
    std::vector<std::string> keywords;
    std::vector<MethodologyInput> inputs;
    std::vector<std::string> outputs;
    std::vector<std::string> metrics;
    std::string issue;
    bool runnable = false;
    bool partial = false;
    std::function<ir::ExecutionGraph(const ShapeRequest&)> build_shape;
    std::function<bool(const ir::ExecutionGraph&)> owns;
    std::function<api::ApiResult(RunRequest&)> run;
};

const std::vector<Methodology>& methodologies();
const Methodology*
find_methodology(std::string_view id_or_model_kind_or_solution);
const Methodology* methodology_for_graph(const ir::ExecutionGraph& graph);

std::string unsupported_methodology_message(const Methodology& methodology);
std::string runnable_methodologies();
nlohmann::json methodology_json(const Methodology& methodology);

} // namespace zetta::ml
