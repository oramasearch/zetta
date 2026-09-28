#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/methodology.hpp"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kTableStart = "<!-- methodology-table:start -->";
constexpr std::string_view kTableEnd = "<!-- methodology-table:end -->";

std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (const std::string& value : values) {
        if (!result.empty()) {
            result += ", ";
        }
        result += value;
    }
    return result.empty() ? "—" : result;
}

std::string roles(const std::vector<zetta::ml::MethodologyInput>& inputs) {
    std::string result;
    for (const zetta::ml::MethodologyInput& input : inputs) {
        if (!result.empty()) {
            result += ", ";
        }
        result += input.role;
        if (input.required) {
            result += "*";
        }
    }
    return result.empty() ? "—" : result;
}

std::string issue_link(const std::string& issue) {
    if (issue.empty()) {
        return "—";
    }
    const std::size_t slash = issue.find_last_of('/');
    const std::string number =
        slash == std::string::npos ? issue : issue.substr(slash + 1);
    return "[#" + number + "](" + issue + ")";
}

std::string methodology_table() {
    std::ostringstream output;
    output
        << "| ID | Methodology | Family | Model kinds | Solutions | Inputs | "
           "Outputs | Metrics | Status | Issue |\n";
    output << "|---|---|---|---|---|---|---|---|---|---|\n";
    for (const zetta::ml::Methodology& methodology :
         zetta::ml::methodologies()) {
        const std::string status = methodology.runnable  ? "runnable"
                                   : methodology.partial ? "partial"
                                                         : "not runnable";
        output << "| " << methodology.id << " | " << methodology.name << " | "
               << methodology.family << " | " << join(methodology.model_kinds)
               << " | " << join(methodology.solutions) << " | "
               << roles(methodology.inputs) << " | "
               << join(methodology.outputs) << " | "
               << join(methodology.metrics) << " | " << status << " | "
               << issue_link(methodology.issue) << " |\n";
    }
    return output.str();
}

std::string generated_section() {
    return std::string(kTableStart) + "\n" + methodology_table() +
           std::string(kTableEnd);
}

} // namespace

TEST(MethodologyRegistry, ListsAllTwentyOneInIdOrder) {
    const auto& methodologies = zetta::ml::methodologies();
    ASSERT_EQ(methodologies.size(), 21u);
    for (std::size_t index = 0; index < methodologies.size(); ++index) {
        const bool runnable = index <= 19;
        EXPECT_EQ(methodologies[index].id, "M-" + std::to_string(index + 1));
        EXPECT_EQ(methodologies[index].runnable, runnable);
        EXPECT_EQ(static_cast<bool>(methodologies[index].build_shape),
                  runnable);
        EXPECT_EQ(static_cast<bool>(methodologies[index].owns), runnable);
        EXPECT_EQ(static_cast<bool>(methodologies[index].run), runnable);
        if (!runnable && index > 0) {
            EXPECT_EQ(methodologies[index].issue,
                      "https://github.com/oramasearch/zetta/issues/" +
                          std::to_string(1040 + index));
        }
    }
    EXPECT_TRUE(methodologies.front().runnable);
    EXPECT_FALSE(methodologies.front().partial);
    EXPECT_EQ(methodologies.front().id, "M-1");
    EXPECT_TRUE(methodologies[19].runnable);
    EXPECT_EQ(methodologies[19].model_kinds.front(), "synthetic_private");
    EXPECT_TRUE(methodologies[16].runnable);
    EXPECT_TRUE(methodologies[15].runnable);
    EXPECT_EQ(methodologies[15].id, "M-16");
    EXPECT_TRUE(methodologies[14].runnable);
    EXPECT_TRUE(methodologies[14].issue.empty());
    EXPECT_TRUE(methodologies[9].runnable);
    EXPECT_FALSE(methodologies[9].partial);
    EXPECT_TRUE(methodologies[10].runnable);
    EXPECT_EQ(methodologies[10].model_kinds.front(), "document_extraction");
    EXPECT_TRUE(methodologies[12].runnable);
    EXPECT_TRUE(methodologies[12].issue.empty());
}

TEST(MethodologyRegistry, VisionBuildsItsOwnRunnableShape) {
    const zetta::ml::Methodology* vision = zetta::ml::find_methodology("M-13");
    ASSERT_NE(vision, nullptr);
    const zetta::ir::ExecutionGraph graph = vision->build_shape(
        {.project = "inspection",
         .intent = "classify defects",
         .hints = {{"database", "factory"},
                   {"table", "frames"},
                   {"backbone_model", "vit-small"}}});
    EXPECT_TRUE(vision->owns(graph));
    ASSERT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Connect).size(), 1u);
    ASSERT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Model).size(), 1u);
    const auto connect =
        graph.nodes_of_kind(zetta::ir::StepKind::Connect).front();
    const auto model = graph.nodes_of_kind(zetta::ir::StepKind::Model).front();
    EXPECT_EQ(connect->attributes.at("source"), "factory");
    EXPECT_EQ(connect->attributes.at("table"), "frames");
    EXPECT_EQ(model->attributes.at("model_kind"), "vision");
    EXPECT_EQ(model->attributes.at("backbone_model"), "vit-small");
    EXPECT_EQ(model->attributes.at("source_disjoint"), true);
}

TEST(MethodologyRegistry, RankBuildsItsOwnRunnableShape) {
    const zetta::ml::Methodology* methodology =
        zetta::ml::find_methodology("M-9");
    ASSERT_NE(methodology, nullptr);
    ASSERT_TRUE(methodology->runnable);

    zetta::ml::ShapeRequest request;
    request.project = "sales";
    request.intent = "Prioritize leads for each representative";
    request.hints = {{"source", "sales"},
                     {"table", "leads"},
                     {"entity", "lead_id"},
                     {"group", "rep_id"},
                     {"label", "relevance"},
                     {"features", {"intent", "fit"}},
                     {"cutoff", 5}};
    const zetta::ir::ExecutionGraph graph =
        methodology->build_shape(request);

    ASSERT_TRUE(methodology->owns(graph));
    const auto models = graph.nodes_of_kind(zetta::ir::StepKind::Model);
    ASSERT_EQ(models.size(), 1u);
    EXPECT_EQ(models.front()->attributes.at("model_kind"), "rank");
    EXPECT_EQ(models.front()->attributes.at("group_column"), "rep_id");
    EXPECT_EQ(models.front()->attributes.at("split_strategy"), "group");
    EXPECT_EQ(models.front()->attributes.at("feature_availability"),
              "ranking_time");
}

TEST(MethodologyRegistry, PrimarySolutionsHaveOneOwner) {
    std::map<std::string, std::string> owners;
    for (const zetta::ml::Methodology& methodology : zetta::ml::methodologies()) {
        for (const std::string& solution : methodology.solutions) {
            const auto [owner, inserted] = owners.emplace(solution, methodology.id);
            EXPECT_TRUE(inserted) << solution << " claimed by " << owner->second
                                  << " and " << methodology.id;
        }
    }
}

TEST(MethodologyRegistry, LanguageBuildsItsOwnGroundedPipeline) {
    const zetta::ml::Methodology* methodology =
        zetta::ml::find_methodology("M-10");
    ASSERT_NE(methodology, nullptr);
    ASSERT_TRUE(methodology->runnable);
    const zetta::ir::ExecutionGraph graph = methodology->build_shape(
        {.project = "language",
         .intent = "answer from documents",
         .hints = {{"dataset_path", "knowledge.chunks"}}});
    EXPECT_TRUE(methodology->owns(graph));
    ASSERT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Connect).size(), 1U);
    ASSERT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Model).size(), 1U);
    ASSERT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Serve).size(), 1U);
    const auto connect =
        graph.nodes_of_kind(zetta::ir::StepKind::Connect).front();
    EXPECT_EQ(connect->attributes.at("source"), "knowledge");
    EXPECT_EQ(connect->attributes.at("table"), "chunks");
    const auto model =
        graph.nodes_of_kind(zetta::ir::StepKind::Model).front();
    EXPECT_EQ(model->attributes.at("model_kind"), "language");
    EXPECT_TRUE(model->attributes.at("governance_check"));
    EXPECT_TRUE(model->attributes.at("citations_required"));
    EXPECT_TRUE(model->attributes.at("refuse_without_retrieval"));
}

TEST(MethodologyRegistry, SpeechBuildsItsOwnRunnableShape) {
  const auto *methodology = zetta::ml::find_methodology("M-14");
  ASSERT_NE(methodology, nullptr);
  ASSERT_TRUE(methodology->runnable);
  const auto graph = methodology->build_shape(
      {.project = "project",
       .intent = "transcribe recordings",
       .hints = {{"database", "media"},
                 {"table", "calls"},
                 {"checkpoint_model", "whisper-tiny"}}});
  EXPECT_TRUE(methodology->owns(graph));
  ASSERT_EQ(graph.node_count(), 4u);
  const auto model = graph.find_node("transcribe_audio");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->attributes.at("model_kind"), "speech");
  EXPECT_TRUE(model->attributes.at("consent_required").get<bool>());
  EXPECT_TRUE(model->attributes.at("redact_pii").get<bool>());
  EXPECT_TRUE(model->attributes.at("audio_local_only").get<bool>());
}

TEST(MethodologyRegistry, ResolvesIdsModelKindsAndPrimarySolutions) {
    ASSERT_NE(zetta::ml::find_methodology("M-3"), nullptr);
    ASSERT_NE(zetta::ml::find_methodology("M-2"), nullptr);
    EXPECT_TRUE(zetta::ml::find_methodology("M-2")->runnable);
    EXPECT_EQ(zetta::ml::find_methodology("entity_resolution")->id, "M-2");
    EXPECT_EQ(zetta::ml::find_methodology("segmentation")->id, "M-3");
    EXPECT_EQ(zetta::ml::find_methodology("S-1")->id, "M-1");
    EXPECT_EQ(zetta::ml::find_methodology("synthetic_private")->id, "M-20");
    EXPECT_EQ(zetta::ml::find_methodology("label-efficient-training")->id,
              "M-19");
    EXPECT_EQ(zetta::ml::find_methodology("stacking")->id, "M-18");
    EXPECT_EQ(zetta::ml::find_methodology("bayesian")->id, "M-17");
    EXPECT_EQ(zetta::ml::find_methodology("S-4")->id, "M-1");
    EXPECT_EQ(zetta::ml::find_methodology("optimization")->id, "M-16");
    EXPECT_EQ(zetta::ml::find_methodology("uplift")->id, "M-6");
    EXPECT_EQ(zetta::ml::find_methodology("S-11")->id, "M-8");
    EXPECT_EQ(zetta::ml::find_methodology("M-15")->id, "M-15");
    EXPECT_EQ(zetta::ml::find_methodology("M-6")->id, "M-6");
    EXPECT_EQ(zetta::ml::find_methodology("graph")->id, "M-12");
    EXPECT_EQ(zetta::ml::find_methodology("S-10")->id, "M-1");
    EXPECT_EQ(zetta::ml::find_methodology("S-12")->id, "M-1");
    EXPECT_EQ(zetta::ml::find_methodology("S-8")->id, "M-7");
    EXPECT_EQ(zetta::ml::find_methodology("document_automation")->id, "M-11");
}

TEST(MethodologyRegistry, M15BuildsAndOwnsItsBanditPipeline) {
    const zetta::ml::Methodology* methodology =
        zetta::ml::find_methodology("M-15");
    ASSERT_NE(methodology, nullptr);
    ASSERT_TRUE(methodology->runnable);

    zetta::ml::ShapeRequest request;
    request.hints = {{"source", "experiments"},
                     {"table", "decisions"},
                     {"arms", {"control", "best"}},
                     {"exploration_floor", 0.1},
                     {"default_arm_cap", 0.9}};
    const zetta::ir::ExecutionGraph graph = methodology->build_shape(request);
    EXPECT_EQ(graph.node_count(), 5u);
    EXPECT_EQ(graph.edge_count(), 4u);
    ASSERT_TRUE(methodology->owns(graph));

    const auto sources = graph.nodes_of_kind(zetta::ir::StepKind::Connect);
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources.front()->attributes.at("source"), "experiments");
    EXPECT_EQ(sources.front()->attributes.at("table"), "decisions");
    const auto models = graph.nodes_of_kind(zetta::ir::StepKind::Model);
    ASSERT_EQ(models.size(), 1u);
    EXPECT_EQ(models.front()->attributes.at("model_kind"), "bandit");
    EXPECT_EQ(models.front()->attributes.at("arms").size(), 2u);
}

TEST(MethodologyRegistry, M16BuildsAndOwnsItsOptimizationShape) {
  const zetta::ml::Methodology *methodology =
      zetta::ml::find_methodology("M-16");
  ASSERT_NE(methodology, nullptr);
  ASSERT_TRUE(methodology->build_shape);
  const zetta::ir::ExecutionGraph graph = methodology->build_shape(
      {.project = "operations",
       .intent = "allocate replenishment under capacity",
       .hints = {{"source", "operations"}, {"table", "decisions"}}});
  EXPECT_TRUE(methodology->owns(graph));
  EXPECT_EQ(zetta::ml::methodology_for_graph(graph), methodology);
  const auto models = graph.nodes_of_kind(zetta::ir::StepKind::Model);
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front()->attributes.at("model_kind"), "optimization");
  EXPECT_TRUE(models.front()->attributes.at("verify_constraints"));
}

TEST(MethodologyRegistry, ResolvesGraphsWithoutClaimingUnknownModelKinds) {
    zetta::ir::ExecutionGraph segmentation;
    segmentation.add_node(zetta::ir::StepKind::Connect, "source", {}, "source");
    segmentation.add_node(zetta::ir::StepKind::Clean, "features", {},
                          "features");
    segmentation.add_node(zetta::ir::StepKind::Model, "segments",
                          nlohmann::json{{"model_kind", "segmentation"}},
                          "segments");
    segmentation.add_edge("source", "features");
    segmentation.add_edge("features", "segments");

    const zetta::ml::Methodology* methodology =
        zetta::ml::methodology_for_graph(segmentation);
    ASSERT_NE(methodology, nullptr);
    EXPECT_EQ(methodology->id, "M-3");
    EXPECT_TRUE(methodology->runnable);

    zetta::ir::ExecutionGraph bayesian;
    bayesian.add_node(zetta::ir::StepKind::Connect, "source", {}, "bayes-source");
    bayesian.add_node(zetta::ir::StepKind::Model, "posterior",
                      nlohmann::json{{"model_kind", "bayesian"}},
                      "posterior");
    bayesian.add_edge("bayes-source", "posterior");
    methodology = zetta::ml::methodology_for_graph(bayesian);
    ASSERT_NE(methodology, nullptr);
    EXPECT_EQ(methodology->id, "M-17");
    EXPECT_TRUE(methodology->runnable);

    zetta::ir::ExecutionGraph ensemble;
    ensemble.add_node(zetta::ir::StepKind::Connect, "source", {}, "ensemble-source");
    ensemble.add_node(zetta::ir::StepKind::Model, "stack",
                      nlohmann::json{{"model_kind", "ensemble"}},
                      "ensemble-model");
    ensemble.add_edge("ensemble-source", "ensemble-model");
    const zetta::ml::Methodology* ensemble_methodology =
        zetta::ml::methodology_for_graph(ensemble);
    ASSERT_NE(ensemble_methodology, nullptr);
    EXPECT_EQ(ensemble_methodology->id, "M-18");
    EXPECT_TRUE(ensemble_methodology->runnable);

    zetta::ir::ExecutionGraph unknown;
    unknown.add_node(zetta::ir::StepKind::Model, "unknown model",
                     nlohmann::json{{"model_kind", "unknown"}}, "unknown");
    EXPECT_EQ(zetta::ml::methodology_for_graph(unknown), nullptr);
}

TEST(MethodologyRegistry, M6BuildsAndOwnsItsCausalPipeline) {
    const zetta::ml::Methodology* methodology =
        zetta::ml::find_methodology("M-6");
    ASSERT_NE(methodology, nullptr);
    zetta::ml::ShapeRequest request;
    request.hints = {{"source", "experiments"},
                     {"table", "population"},
                     {"entity", "customer_id"},
                     {"treatment", "offer"},
                     {"outcome", "converted"}};
    const zetta::ir::ExecutionGraph graph = methodology->build_shape(request);
    ASSERT_EQ(graph.node_count(), 4u);
    EXPECT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Connect).size(), 1u);
    EXPECT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Clean).size(), 1u);
    EXPECT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Model).size(), 1u);
    EXPECT_EQ(graph.nodes_of_kind(zetta::ir::StepKind::Act).size(), 1u);
    const auto model = graph.nodes_of_kind(zetta::ir::StepKind::Model).front();
    EXPECT_EQ(model->attributes.at("model_kind"), "uplift");
    EXPECT_EQ(model->attributes.at("treatment"), "offer");
    EXPECT_EQ(model->attributes.at("outcome"), "converted");
    EXPECT_TRUE(methodology->owns(graph));
    EXPECT_FALSE(zetta::ml::find_methodology("M-1")->owns(graph));
}

TEST(MethodologyRegistry, DocumentationTableMatchesTheRegistry) {
    const fs::path path =
        fs::path(ZETTA_SOURCE_DIR) / "docs" / "ml" / "README.md";
    std::ifstream input(path);
    ASSERT_TRUE(input.good()) << path;
    std::string document((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());

    const std::size_t start = document.find(kTableStart);
    const std::size_t end = document.find(kTableEnd);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    ASSERT_LT(start, end);
    const std::size_t section_size = end + kTableEnd.size() - start;

    if (std::getenv("ZETTA_WRITE_METHODOLOGIES") != nullptr) {
        document.replace(start, section_size, generated_section());
        std::ofstream output(path, std::ios::trunc);
        ASSERT_TRUE(output.good()) << path;
        output << document;
        return;
    }

    EXPECT_EQ(document.substr(start, section_size), generated_section())
        << "regenerate with ZETTA_WRITE_METHODOLOGIES=1 "
           "./build/tests/zetta-tests --gtest_filter="
           "MethodologyRegistry.DocumentationTableMatchesTheRegistry";
}
