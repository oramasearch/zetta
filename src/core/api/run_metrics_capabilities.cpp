// SPDX-License-Identifier: MIT

// Run metrics on the control surface (#1137; docs/ml/run-metrics.md).

#include <map>
#include <set>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json run_property() {
    return {{"type", "string"}, {"minLength", 1}};
}

nlohmann::json metric_name_property() {
    return {{"type", "string"},
            {"pattern", "^[A-Za-z0-9_.-]{1,128}$"}};
}

nlohmann::json metric_values_schema() {
    return {{"type", "object"},
            {"minProperties", 1},
            {"patternProperties",
             {{"^[A-Za-z0-9_.-]{1,128}$", {{"type", "number"}}}}},
            {"additionalProperties", false}};
}

const nlohmann::json& stored_metrics_schema() {
    return zetta::pipeline::run_value_schema()
        .at("properties")
        .at("metrics");
}

const nlohmann::json& log_metrics_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run", "step", "values"}},
        {"additionalProperties", false},
        {"properties",
         {{"run", run_property()},
          {"step", {{"type", "integer"}, {"minimum", 0}}},
          {"values", metric_values_schema()},
          {"time", {{"type", "string"}, {"minLength", 1}}}}},
    };
    return schema;
}

const nlohmann::json& log_metrics_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run", "step", "topic"}},
        {"additionalProperties", false},
        {"properties",
         {{"run", run_property()},
          {"step", {{"type", "integer"}, {"minimum", 0}}},
          {"topic", {{"type", "string"}}}}},
    };
    return schema;
}

const nlohmann::json& log_params_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run", "params"}},
        {"additionalProperties", false},
        {"properties",
         {{"run", run_property()},
          {"params", stored_metrics_schema().at("properties").at("params")}}},
    };
    return schema;
}

const nlohmann::json& log_params_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run", "params"}},
        {"additionalProperties", false},
        {"properties",
         {{"run", run_property()},
          {"params", stored_metrics_schema().at("properties").at("params")}}},
    };
    return schema;
}

const nlohmann::json& log_artifact_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run", "artifact"}},
        {"additionalProperties", false},
        {"properties",
         {{"run", run_property()},
          {"artifact",
           stored_metrics_schema()
               .at("properties")
               .at("artifacts")
               .at("items")}}},
    };
    return schema;
}

const nlohmann::json& log_artifact_output_schema() {
    static const nlohmann::json schema = log_artifact_schema();
    return schema;
}

const nlohmann::json& get_metrics_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run"}},
        {"additionalProperties", false},
        {"properties",
         {{"run", run_property()},
          {"names",
           {{"type", "array"},
            {"uniqueItems", true},
            {"items", metric_name_property()}}},
          {"from_step", {{"type", "integer"}, {"minimum", 0}}},
          {"max_points",
           {{"type", "integer"}, {"minimum", 4}, {"maximum", 10'000}}}}},
    };
    return schema;
}

nlohmann::json point_schema() {
    return {{"type", "object"},
            {"required", {"step", "value", "time"}},
            {"additionalProperties", false},
            {"properties",
             {{"step", {{"type", "integer"}, {"minimum", 0}}},
              {"value", {{"type", "number"}}},
              {"time", {{"type", "string"}}}}}};
}

const nlohmann::json& get_metrics_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json series_item;
        series_item["type"] = "object";
        series_item["required"] = {"name", "points"};
        series_item["additionalProperties"] = false;
        series_item["properties"]["name"] = metric_name_property();
        series_item["properties"]["points"] = {
            {"type", "array"}, {"items", point_schema()}};

        nlohmann::json document;
        document["type"] = "object";
        document["required"] = {"run", "params", "summary", "artifacts",
                                  "series"};
        document["additionalProperties"] = false;
        document["properties"]["run"] = run_property();
        document["properties"]["metrics_suppressed"] =
            pipeline::run_value_schema().at("properties").at("metrics_suppressed");
        document["properties"]["params"] =
            stored_metrics_schema().at("properties").at("params");
        document["properties"]["summary"] =
            stored_metrics_schema().at("properties").at("summary");
        document["properties"]["artifacts"] =
            stored_metrics_schema().at("properties").at("artifacts");
        document["properties"]["series"] = {
            {"type", "array"}, {"items", std::move(series_item)}};
        return document;
    }();
    return schema;
}

const nlohmann::json& compare_runs_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"runs", "names"}},
        {"additionalProperties", false},
        {"properties",
         {{"runs",
           {{"type", "array"},
            {"minItems", 2},
            {"uniqueItems", true},
            {"items", run_property()}}},
          {"names",
           {{"type", "array"},
            {"minItems", 1},
            {"uniqueItems", true},
            {"items", metric_name_property()}}}}},
    };
    return schema;
}

const nlohmann::json& compare_runs_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json aligned_value;
        aligned_value["type"] = "object";
        aligned_value["required"] = {"run", "value", "time"};
        aligned_value["additionalProperties"] = false;
        aligned_value["properties"]["run"] = run_property();
        aligned_value["properties"]["value"] = {{"type", "number"}};
        aligned_value["properties"]["time"] = {{"type", "string"}};

        nlohmann::json aligned_point;
        aligned_point["type"] = "object";
        aligned_point["required"] = {"step", "values"};
        aligned_point["additionalProperties"] = false;
        aligned_point["properties"]["step"] = {
            {"type", "integer"}, {"minimum", 0}};
        aligned_point["properties"]["values"] = {
            {"type", "array"}, {"items", std::move(aligned_value)}};

        nlohmann::json metric;
        metric["type"] = "object";
        metric["required"] = {"name", "points"};
        metric["additionalProperties"] = false;
        metric["properties"]["name"] = metric_name_property();
        metric["properties"]["points"] = {
            {"type", "array"}, {"items", std::move(aligned_point)}};

        nlohmann::json document;
        document["type"] = "object";
        document["required"] = {"runs", "metrics"};
        document["additionalProperties"] = false;
        document["properties"]["runs"] = {
            {"type", "array"}, {"items", run_property()}};
        document["properties"]["metrics"] = {
            {"type", "array"}, {"items", std::move(metric)}};
        return document;
    }();
    return schema;
}

ApiResult handle_log_metrics(const nlohmann::json& input,
                             ApiContext& context) {
    std::map<std::string, double> values;
    for (const auto& [name, value] : input.at("values").items()) {
        values.emplace(name, value.get<double>());
    }
    const std::string run = input.at("run").get<std::string>();
    const std::uint64_t step = input.at("step").get<std::uint64_t>();
    std::string reason;
    if (!context.node.log_run_metrics(
            run, step, std::move(values),
            input.value("time", std::string{}), &reason)) {
        const bool missing = reason.starts_with("/run: no run");
        return ApiResult::failure(missing ? "not_found" : "invalid_input",
                                  reason, missing ? "/run" : "");
    }
    return ApiResult::success(
        {{"run", run},
         {"step", step},
         {"topic", "run-metrics." + run}});
}

ApiResult handle_log_params(const nlohmann::json& input,
                            ApiContext& context) {
    const std::string run = input.at("run").get<std::string>();
    std::string reason;
    if (!context.node.log_run_params(run, input.at("params"), &reason)) {
        const bool missing = reason.starts_with("/run: no run");
        return ApiResult::failure(missing ? "not_found" : "invalid_input",
                                  reason, missing ? "/run" : "/params");
    }
    return ApiResult::success({{"run", run}, {"params", input.at("params")}});
}

ApiResult handle_log_artifact(const nlohmann::json& input,
                              ApiContext& context) {
    const std::string run = input.at("run").get<std::string>();
    nlohmann::json artifact = input.at("artifact");
    std::string reason;
    if (!context.node.log_run_artifact(run, artifact, &reason)) {
        const bool missing = reason.starts_with("/run: no run");
        return ApiResult::failure(missing ? "not_found" : "invalid_input",
                                  reason, missing ? "/run" : "/artifact");
    }
    if (!artifact.contains("time")) {
        const std::shared_ptr<zetta::pipeline::Run> stored =
            context.node.find_run(run);
        artifact = stored->value().metrics.at("artifacts").back();
    }
    return ApiResult::success({{"run", run}, {"artifact", artifact}});
}

ApiResult handle_get_metrics(const nlohmann::json& input,
                             ApiContext& context) {
    const std::string run_id = input.at("run").get<std::string>();
    const std::shared_ptr<zetta::pipeline::Run> run =
        context.node.find_run(run_id);
    if (!run) {
        return ApiResult::failure("not_found", "no run with id '" + run_id +
                                                   "'", "/run");
    }
    const std::vector<std::string> names =
        input.value("names", std::vector<std::string>{});
    std::optional<std::size_t> max_points;
    if (input.contains("max_points")) {
        max_points = input.at("max_points").get<std::size_t>();
    }
    return ApiResult::success(run->get_metrics(
        names, input.value("from_step", std::uint64_t{0}), max_points));
}

ApiResult handle_compare_runs(const nlohmann::json& input,
                              ApiContext& context) {
    const std::vector<std::string> run_ids =
        input.at("runs").get<std::vector<std::string>>();
    const std::vector<std::string> names =
        input.at("names").get<std::vector<std::string>>();
    std::map<std::string,
             std::map<std::uint64_t, nlohmann::json>> aligned;
    for (const std::string& run_id : run_ids) {
        const std::shared_ptr<zetta::pipeline::Run> run =
            context.node.find_run(run_id);
        if (!run) {
            return ApiResult::failure("not_found", "no run with id '" + run_id +
                                                       "'", "/runs");
        }
        const nlohmann::json metrics = run->get_metrics(names);
        for (const nlohmann::json& series : metrics.at("series")) {
            const std::string name = series.at("name").get<std::string>();
            for (const nlohmann::json& point : series.at("points")) {
                const std::uint64_t step =
                    point.at("step").get<std::uint64_t>();
                nlohmann::json& values = aligned[name][step];
                if (values.is_null()) {
                    values = nlohmann::json::array();
                }
                values.push_back({{"run", run_id},
                                  {"value", point.at("value")},
                                  {"time", point.at("time")}});
            }
        }
    }
    nlohmann::json metrics = nlohmann::json::array();
    for (const std::string& name : names) {
        nlohmann::json points = nlohmann::json::array();
        for (const auto& [step, values] : aligned[name]) {
            points.push_back({{"step", step}, {"values", values}});
        }
        metrics.push_back({{"name", name}, {"points", std::move(points)}});
    }
    return ApiResult::success(
        {{"runs", run_ids}, {"metrics", std::move(metrics)}});
}

}  // namespace

void register_run_metrics_capabilities(Registry& registry) {
    registry.add(Capability{
        "log_run_metrics",
        "Log run metrics",
        "Append one step of numeric metrics to a Run, update its summary and "
        "publish the batch live to run-metrics.<run>.",
        log_metrics_schema(),
        log_metrics_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "log_run_metrics"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_log_metrics});
    registry.add(Capability{
        "log_run_params",
        "Log run parameters",
        "Record a Run's flattened dotted-key parameters once; this mutates "
        "the replicated Run.",
        log_params_schema(),
        log_params_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "log_run_params"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_log_params});
    registry.add(Capability{
        "log_run_artifact",
        "Log run artifact",
        "Append typed JSON chart data to a Run; this mutates the replicated Run.",
        log_artifact_schema(),
        log_artifact_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "log_run_artifact"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_log_artifact});
    registry.add(Capability{
        "get_run_metrics",
        "Get run metrics",
        "Read selected Run metric series and summaries, with optional "
        "extrema-preserving LTTB downsampling. No side effects.",
        get_metrics_schema(),
        get_metrics_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_metrics});
    registry.add(Capability{
        "compare_runs",
        "Compare runs",
        "Read named metric series from multiple Runs aligned by step. No side effects.",
        compare_runs_schema(),
        compare_runs_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_compare_runs});
}

}  // namespace zetta::api
