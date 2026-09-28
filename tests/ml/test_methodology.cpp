#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        EXPECT_EQ(methodologies[index].id, "M-" + std::to_string(index + 1));
        const bool implemented = index == 0 || index == 17;
        EXPECT_EQ(methodologies[index].runnable, implemented);
        EXPECT_EQ(static_cast<bool>(methodologies[index].build_shape),
                  implemented);
        EXPECT_EQ(static_cast<bool>(methodologies[index].owns), implemented);
        EXPECT_EQ(static_cast<bool>(methodologies[index].run), implemented);
        if (index > 0 && index != 17) {
            EXPECT_EQ(methodologies[index].issue,
                      "https://github.com/oramasearch/zetta/issues/" +
                          std::to_string(1040 + index));
        }
    }
    EXPECT_TRUE(methodologies.front().runnable);
    EXPECT_FALSE(methodologies.front().partial);
    EXPECT_EQ(methodologies.front().id, "M-1");
    EXPECT_FALSE(methodologies[9].runnable);
    EXPECT_TRUE(methodologies[9].partial);
}

TEST(MethodologyRegistry, ResolvesIdsModelKindsAndPrimarySolutions) {
    ASSERT_NE(zetta::ml::find_methodology("M-3"), nullptr);
    EXPECT_EQ(zetta::ml::find_methodology("segmentation")->id, "M-3");
    EXPECT_EQ(zetta::ml::find_methodology("S-1")->id, "M-1");
    EXPECT_EQ(zetta::ml::find_methodology("S-12")->id, "M-9");
    EXPECT_EQ(zetta::ml::find_methodology("S-10")->id, "M-18");
    EXPECT_EQ(zetta::ml::find_methodology("stacking")->id, "M-18");
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
    EXPECT_FALSE(methodology->runnable);

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
