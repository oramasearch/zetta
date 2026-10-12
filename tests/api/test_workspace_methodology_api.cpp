// Workspace methodologies (zetta#1261, lake#655) end to end through the registry,
// against a replicated node: the read-only catalogue, register/get/update/delete,
// list_methodologies marking source/editable/runnable, the pipeline step compiled
// from a W- id, Verification refusing it until validated, run handing it to Lake's
// runner, and record_methodology_run registering a Model with held-out metrics.
#include <gtest/gtest.h>

#include <string>

#include "../../src/core/base64.hpp"
#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/ml/workspace_methodology.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/sha256.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

struct WorkspaceMethodologyFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"workspace-methodology-node"};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false},
                     &replication};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }

    static nlohmann::json churn_methodology() {
        return {{"name", "Gradient boosted churn"},
                {"description", "scikit-learn gradient boosting on the tabular rows"},
                {"input_kinds", {"tabular"}},
                {"target", {{"required", true}, {"kind", "binary"}}},
                {"parameters",
                 {{{"name", "learning_rate"}, {"kind", "float"}, {"default", 0.1},
                   {"minimum", 0.001}, {"maximum", 1.0}, {"log", true}},
                  {{"name", "max_depth"}, {"kind", "int"}, {"default", 6},
                   {"minimum", 1}, {"maximum", 16}}}},
                {"metrics", {"auc", "log_loss"}},
                {"code", {{"repository_id", "repo-1"}, {"revision", "rev-a"}, {"path", "."}}}};
    }

    std::string register_churn() {
        const ApiResult created = call("register_methodology", churn_methodology());
        EXPECT_TRUE(created.ok) << created.error.message;
        return created.value.at("methodology").at("id").get<std::string>();
    }

    void validate(const std::string& id, const std::string& revision) {
        const ApiResult validated =
            call("update_methodology",
                 {{"id", id},
                  {"validation",
                   {{"status", "passed"},
                    {"findings", {{{"check", "interface.fit"}, {"ok", true}}}},
                    {"run_id", "lake-run-validate"},
                    {"revision", revision},
                    {"validated_at", "2026-10-11T12:00:00Z"}}}});
        ASSERT_TRUE(validated.ok) << validated.error.message;
    }
};

TEST_F(WorkspaceMethodologyFixture, OramasMethodologiesAreReadOnly) {
    const ApiResult updated = call("update_methodology", {{"id", "M-1"}, {"name", "Mine now"}});
    ASSERT_FALSE(updated.ok);
    EXPECT_EQ(updated.error.code, "forbidden");
    EXPECT_NE(updated.error.message.find("Duplicate it"), std::string::npos)
        << updated.error.message;
    EXPECT_EQ(zetta::api::rest_status(updated), 403);

    const ApiResult deleted = call("delete_methodology", {{"id", "M-3"}});
    ASSERT_FALSE(deleted.ok);
    EXPECT_EQ(deleted.error.code, "forbidden");

    const ApiResult got = call("get_methodology", {{"id", "M-1"}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value.at("methodology").at("source"), "orama");
    EXPECT_FALSE(got.value.at("methodology").at("editable").get<bool>());
    EXPECT_EQ(got.value.at("methodology").at("name"), "Tabular supervised learning");
}

TEST_F(WorkspaceMethodologyFixture, RegisterGetUpdateDeleteAndList) {
    const ApiResult created = call("register_methodology", churn_methodology());
    ASSERT_TRUE(created.ok) << created.error.message;
    const nlohmann::json& entry = created.value.at("methodology");
    const std::string id = entry.at("id").get<std::string>();
    EXPECT_TRUE(zetta::ml::is_workspace_methodology_id(id)) << id;
    EXPECT_EQ(entry.at("source"), "workspace");
    EXPECT_TRUE(entry.at("editable").get<bool>());
    EXPECT_FALSE(entry.at("runnable").get<bool>());
    EXPECT_EQ(entry.at("validation").at("status"), "not_validated");
    EXPECT_EQ(entry.at("model_kinds"), nlohmann::json::array({"python:" + id}));
    EXPECT_EQ(entry.at("metrics"), nlohmann::json::array({"held_out_auc", "held_out_log_loss"}));
    EXPECT_EQ(entry.at("interface").at("metrics"), nlohmann::json::array({"auc", "log_loss"}));
    EXPECT_EQ(entry.at("family"), "Workspace");
    EXPECT_FALSE(entry.at("studies").get<bool>());

    // Listed after the catalogue, every entry marked.
    const ApiResult listed = call("list_methodologies", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    const nlohmann::json& all = listed.value.at("methodologies");
    ASSERT_EQ(all.size(), 22u);
    for (std::size_t index = 0; index < 21; ++index) {
        EXPECT_EQ(all[index].at("source"), "orama");
        EXPECT_FALSE(all[index].at("editable").get<bool>());
    }
    EXPECT_EQ(all[21].at("id"), id);
    EXPECT_EQ(all[21].at("source"), "workspace");

    // Validation passed for the current revision makes it runnable.
    validate(id, "rev-a");
    ApiResult got = call("get_methodology", {{"id", id}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_TRUE(got.value.at("methodology").at("runnable").get<bool>());

    // New code is not validated code.
    const ApiResult moved = call(
        "update_methodology",
        {{"id", id},
         {"code", {{"repository_id", "repo-1"}, {"revision", "rev-b"}, {"path", "."}}},
         {"description", "now with more trees"}});
    ASSERT_TRUE(moved.ok) << moved.error.message;
    EXPECT_FALSE(moved.value.at("methodology").at("runnable").get<bool>());
    EXPECT_EQ(moved.value.at("methodology").at("validation").at("status"), "not_validated");
    EXPECT_EQ(moved.value.at("methodology").at("description"), "now with more trees");
    EXPECT_EQ(moved.value.at("methodology").at("created_at"), entry.at("created_at"));

    // A default outside its bounds is refused, and nothing changes.
    nlohmann::json bad_parameters = churn_methodology().at("parameters");
    bad_parameters[1]["default"] = 40;
    const ApiResult refused =
        call("update_methodology", {{"id", id}, {"parameters", bad_parameters}});
    ASSERT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_NE(refused.error.message.find("above the maximum"), std::string::npos)
        << refused.error.message;

    // A target of kind none must not be required.
    nlohmann::json inconsistent = churn_methodology();
    inconsistent["target"] = {{"required", true}, {"kind", "none"}};
    EXPECT_EQ(call("register_methodology", inconsistent).error.code, "invalid_input");

    // Duplicating names a methodology that exists.
    nlohmann::json duplicate = churn_methodology();
    duplicate["duplicated_from"] = "M-1";
    EXPECT_TRUE(call("register_methodology", duplicate).ok);
    duplicate["duplicated_from"] = "W-00000000";
    EXPECT_EQ(call("register_methodology", duplicate).error.code, "invalid_input");

    const ApiResult removed = call("delete_methodology", {{"id", id}});
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_TRUE(removed.value.at("removed").get<bool>());
    EXPECT_FALSE(call("delete_methodology", {{"id", id}}).value.at("removed").get<bool>());
    EXPECT_EQ(call("get_methodology", {{"id", id}}).error.code, "not_found");
}

TEST_F(WorkspaceMethodologyFixture, APipelineStepIsVerifiedOnlyOnceValidatedAndRunsInLakesRunner) {
    const std::string id = register_churn();
    ASSERT_TRUE(call("create_project", {{"name", "churn"}}).ok);
    ASSERT_TRUE(call("capture_intent", {{"project", "churn"}, {"text", "Predict who cancels"}}).ok);
    const ApiResult compiled =
        call("compile_intent", {{"project", "churn"},
                                {"methodology", id},
                                {"name", "churn with my code"},
                                {"hints",
                                 {{"dataset_path", "shop.customers"},
                                  {"label", "churned"},
                                  {"params", {{"max_depth", 4}}}}}});
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    EXPECT_EQ(compiled.value.at("methodology"), id);
    EXPECT_FALSE(compiled.value.at("runnable").get<bool>());

    const ApiResult graph = call("get_graph", {{"project", "churn"}, {"graph", "churn with my code"}});
    ASSERT_TRUE(graph.ok) << graph.error.message;
    bool found_step = false;
    for (const nlohmann::json& step : graph.value.at("nodes")) {
        if (step.value("id", std::string()) != "python_methodology") continue;
        found_step = true;
        EXPECT_EQ(step.at("attributes").at("model_kind"), "python:" + id);
        EXPECT_EQ(step.at("attributes").at("parameters").at("max_depth"), 4);
        EXPECT_EQ(step.at("attributes").at("parameters").at("learning_rate"), 0.1);
        EXPECT_EQ(step.at("attributes").at("target"), "churned");
    }
    EXPECT_TRUE(found_step) << graph.value.dump();

    ASSERT_TRUE(call("approve_plan", {{"project", "churn"}, {"graph", "churn with my code"}}).ok);

    // Not validated: Verification fails and says to validate it.
    ApiResult gate = call("gate", {{"project", "churn"}, {"graph", "churn with my code"}});
    ASSERT_TRUE(gate.ok) << gate.error.message;
    EXPECT_TRUE(gate.value.at("block_deploy").get<bool>());
    EXPECT_NE(gate.value.dump().find("validate it"), std::string::npos) << gate.value.dump();
    ApiResult run = call("run", {{"project", "churn"}, {"graph", "churn with my code"}});
    ASSERT_FALSE(run.ok);
    EXPECT_NE(run.error.message.find("validate it"), std::string::npos) << run.error.message;

    // Validated: Verification passes, and run hands it to Lake's runner.
    validate(id, "rev-a");
    gate = call("gate", {{"project", "churn"}, {"graph", "churn with my code"}});
    ASSERT_TRUE(gate.ok) << gate.error.message;
    EXPECT_FALSE(gate.value.at("block_deploy").get<bool>()) << gate.value.dump();
    run = call("run", {{"project", "churn"}, {"graph", "churn with my code"}});
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.error.code, "runner_required");
    EXPECT_EQ(zetta::api::rest_status(run), 409);
    const nlohmann::json& details = run.error.details;
    EXPECT_EQ(details.at("methodology"), id);
    EXPECT_EQ(details.at("dataset_path"), "shop.customers");
    EXPECT_EQ(details.at("target"), "churned");
    EXPECT_EQ(details.at("params").at("max_depth"), 4);
    EXPECT_EQ(details.at("graph_name"), "churn with my code");
    EXPECT_EQ(details.at("code").at("revision"), "rev-a");

    // Lake reports the step's result: a Run on the pipeline and a Model.
    const std::string artifact = "pickled-model-bytes";
    const ApiResult recorded = call(
        "record_methodology_run",
        {{"methodology", id},
         {"run_id", "lake-run-1"},
         {"mode", "train"},
         {"dataset_path", "shop.customers"},
         {"params", {{"max_depth", 4}, {"learning_rate", 0.1}}},
         {"metrics", {{"in_sample_auc", 0.93}, {"held_out_auc", 0.84}}},
         {"rows", {{"train", 800}, {"held_out", 200}}},
         {"artifact_base64", zetta::detail::base64_encode_bytes(artifact)},
         {"artifact_format", "pickle"},
         {"project", "churn"},
         {"graph", "churn with my code"}});
    ASSERT_TRUE(recorded.ok) << recorded.error.message;
    const std::string model_id = recorded.value.at("model_id").get<std::string>();
    const std::string run_id = recorded.value.at("run_id").get<std::string>();

    const auto model = node.find_model(model_id);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->value().kind, "python:" + id);
    EXPECT_EQ(model->value().metrics.at("held_out_auc"), 0.84);
    EXPECT_EQ(model->value().run, run_id);
    ASSERT_TRUE(model->value().weights.has_value());
    EXPECT_EQ(model->value().weights->size, artifact.size());
    EXPECT_EQ(model->value().feature_schema.at("lake_run_id"), "lake-run-1");

    const auto produced = node.find_run(run_id);
    ASSERT_TRUE(produced);
    EXPECT_EQ(produced->value().status, zetta::pipeline::RunStatus::Succeeded);
    EXPECT_EQ(produced->value().pipeline_name, "churn with my code");

    const ApiResult models = call("list_models", nlohmann::json::object());
    ASSERT_TRUE(models.ok) << models.error.message;
    EXPECT_NE(models.value.dump().find(model_id), std::string::npos);
}

TEST_F(WorkspaceMethodologyFixture, RecordingARunChecksWhatItIsGiven) {
    const std::string id = register_churn();
    // Validation registers nothing.
    const ApiResult validation =
        call("record_methodology_run", {{"methodology", id}, {"run_id", "v"}, {"mode", "validate"}});
    ASSERT_TRUE(validation.ok) << validation.error.message;
    EXPECT_TRUE(validation.value.at("model_id").is_null());

    // Training reports its artifact, as base64.
    EXPECT_EQ(call("record_methodology_run",
                   {{"methodology", id}, {"run_id", "t"}, {"mode", "train"}})
                  .error.path,
              "/artifact_base64");
    EXPECT_EQ(call("record_methodology_run", {{"methodology", id},
                                              {"run_id", "t"},
                                              {"mode", "train"},
                                              {"artifact_base64", "not base64!"}})
                  .error.code,
              "invalid_input");
    // Metrics are numbers only.
    EXPECT_EQ(call("record_methodology_run", {{"methodology", id},
                                              {"run_id", "t"},
                                              {"mode", "train"},
                                              {"metrics", {{"held_out_auc", "high"}}},
                                              {"artifact_base64", "AAAA"}})
                  .error.code,
              "invalid_input");
    // An unknown methodology.
    EXPECT_EQ(call("record_methodology_run", {{"methodology", "W-0000abcd"},
                                              {"run_id", "t"},
                                              {"mode", "train"},
                                              {"artifact_base64", "AAAA"}})
                  .error.code,
              "not_found");
    // A training run without a pipeline still registers its Model.
    const ApiResult standalone = call("record_methodology_run",
                                      {{"methodology", id},
                                       {"run_id", "t"},
                                       {"mode", "train"},
                                       {"metrics", {{"held_out_auc", 0.8}}},
                                       {"artifact_base64", "AAAA"},
                                       {"artifact_format", "bytes"}});
    ASSERT_TRUE(standalone.ok) << standalone.error.message;
    EXPECT_TRUE(node.find_model(standalone.value.at("model_id").get<std::string>()));
}

// lake#690: an artifact larger than one request moves in chunks, staged until
// record_methodology_run adopts it as the Model's weights, which read back in
// chunks with their size and SHA-256.
TEST_F(WorkspaceMethodologyFixture, AnArtifactMovesInChunksAndReadsBackInChunks) {
    const std::string id = register_churn();
    std::string artifact;
    for (int i = 0; i < 5000; ++i) artifact.push_back(static_cast<char>(i * 31 % 256));
    const auto chunk = [&](std::size_t offset, std::size_t length) {
        return zetta::detail::base64_encode_bytes(std::string_view(artifact).substr(offset, length));
    };
    // In order: an offset past what is staged is a conflict naming where to resume.
    ApiResult first = call("append_methodology_artifact",
                           {{"methodology", id}, {"run_id", "big"}, {"offset", 0},
                            {"chunk_base64", chunk(0, 2000)}});
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_EQ(first.value.at("received"), 2000);
    const ApiResult gap = call("append_methodology_artifact",
                               {{"methodology", id}, {"run_id", "big"}, {"offset", 3000},
                                {"chunk_base64", chunk(3000, 10)}});
    ASSERT_FALSE(gap.ok);
    EXPECT_EQ(gap.error.code, "conflict");
    // A chunk sent again replaces what was held from its offset.
    ASSERT_TRUE(call("append_methodology_artifact",
                     {{"methodology", id}, {"run_id", "big"}, {"offset", 2000},
                      {"chunk_base64", chunk(2000, 2000)}}).ok);
    const ApiResult again = call("append_methodology_artifact",
                                 {{"methodology", id}, {"run_id", "big"}, {"offset", 2000},
                                  {"chunk_base64", chunk(2000, 3000)}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value.at("received"), 5000);
    // Not base64, or an unknown methodology.
    EXPECT_EQ(call("append_methodology_artifact",
                   {{"methodology", id}, {"run_id", "big"}, {"offset", 0},
                    {"chunk_base64", "not base64!"}}).error.code,
              "invalid_input");
    EXPECT_EQ(call("append_methodology_artifact",
                   {{"methodology", "W-0000abcd"}, {"run_id", "big"}, {"offset", 0},
                    {"chunk_base64", "AAAA"}}).error.code,
              "not_found");

    // The record must name what was staged: a wrong size or digest is a conflict.
    const std::string digest = zetta::util::sha256_hex(artifact);
    EXPECT_EQ(call("record_methodology_run",
                   {{"methodology", id}, {"run_id", "big"}, {"mode", "train"},
                    {"artifact_staged", {{"bytes", 4999}, {"sha256", digest}}}}).error.code,
              "conflict");
    EXPECT_EQ(call("record_methodology_run",
                   {{"methodology", id}, {"run_id", "big"}, {"mode", "train"},
                    {"artifact_staged", {{"bytes", 5000}, {"sha256", std::string(64, '0')}}}})
                  .error.code,
              "conflict");
    EXPECT_EQ(call("record_methodology_run",
                   {{"methodology", id}, {"run_id", "big"}, {"mode", "train"},
                    {"artifact_base64", "AAAA"},
                    {"artifact_staged", {{"bytes", 5000}, {"sha256", digest}}}}).error.code,
              "invalid_input");
    EXPECT_EQ(call("record_methodology_run",
                   {{"methodology", id}, {"run_id", "nothing staged"}, {"mode", "train"},
                    {"artifact_staged", {{"bytes", 5000}, {"sha256", digest}}}}).error.code,
              "conflict");
    const ApiResult recorded =
        call("record_methodology_run",
             {{"methodology", id}, {"run_id", "big"}, {"mode", "train"},
              {"metrics", {{"held_out_auc", 0.7}}}, {"artifact_format", "bytes"},
              {"artifact_staged", {{"bytes", 5000}, {"sha256", digest}}}});
    ASSERT_TRUE(recorded.ok) << recorded.error.message;
    const std::string model_id = recorded.value.at("model_id").get<std::string>();
    const auto model = node.find_model(model_id);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->weights()->size, 5000u);
    EXPECT_EQ(model->weights()->sha256, digest);
    EXPECT_EQ(model->read_weights(), artifact);

    // Read back 2000 bytes at a time.
    std::string read;
    nlohmann::json offset = 0;
    int pages = 0;
    while (!offset.is_null()) {
        const ApiResult page = call("read_model_weights",
                                    {{"model_id", model_id}, {"offset", offset}, {"limit", 2000}});
        ASSERT_TRUE(page.ok) << page.error.message;
        EXPECT_EQ(page.value.at("total_bytes"), 5000);
        EXPECT_EQ(page.value.at("sha256"), digest);
        read += zetta::detail::base64_decode_to_string(
            page.value.at("chunk_base64").get<std::string>());
        offset = page.value.at("next_offset");
        ++pages;
    }
    EXPECT_EQ(pages, 3);
    EXPECT_EQ(read, artifact);
    EXPECT_EQ(call("read_model_weights", {{"model_id", model_id}, {"offset", 5001}}).error.code,
              "invalid_input");
    EXPECT_EQ(call("read_model_weights", {{"model_id", "no-such-model"}}).error.code, "not_found");
}

TEST_F(WorkspaceMethodologyFixture, ItsPipelineIsNotDeployedForServing) {
    const std::string id = register_churn();
    validate(id, "rev-a");
    ASSERT_TRUE(call("create_project", {{"name", "serve"}}).ok);
    ASSERT_TRUE(call("capture_intent", {{"project", "serve"}, {"text", "Predict who cancels"}}).ok);
    ASSERT_TRUE(call("compile_intent", {{"project", "serve"}, {"methodology", id}, {"name", "g"}}).ok);
    ASSERT_TRUE(call("approve_plan", {{"project", "serve"}, {"graph", "g"}}).ok);
    const ApiResult deployed = call("deploy", {{"project", "serve"}, {"graph", "g"}});
    ASSERT_FALSE(deployed.ok);
    EXPECT_EQ(deployed.error.code, "not_supported");
    EXPECT_NE(deployed.error.message.find("Lake's runner"), std::string::npos);

    // An unknown workspace methodology does not compile.
    const ApiResult unknown =
        call("compile_intent", {{"project", "serve"}, {"methodology", "W-0000abcd"}, {"name", "h"}});
    ASSERT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error.code, "invalid_input");
}

} // namespace
