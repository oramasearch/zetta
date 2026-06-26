#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "temp_dir.hpp"
#include "zetta/n8n_import.hpp"
#include "zetta/node.hpp"
#include "zetta/project.hpp"
#include "zetta/project_input.hpp"
#include "zetta/sha256.hpp"

namespace fs = std::filesystem;
using zetta::import_n8n_workflow;
using zetta::import_n8n_workflow_file;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ProjectInputKind;
using zetta::ProjectInputSource;
using zetta::test::TempDir;
using zetta::validate_n8n_workflow;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// Read a whole file as raw bytes (binary; no newline translation), so we can
// assert the stored payload round-trips byte-for-byte against the source.
std::string read_raw(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// The churn-workflow fixture shipped under tests/fixtures (a realistic n8n UI
// export, with id/versionId/meta/tags/pinData alongside nodes + connections).
fs::path churn_fixture() {
    return fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn_workflow.json";
}

constexpr const char* kMinimalWorkflow = R"({
  "name": "minimal",
  "nodes": [
    {"id": "1", "name": "Start", "type": "n8n-nodes-base.start",
     "typeVersion": 1, "position": [0, 0], "parameters": {}}
  ],
  "connections": {}
})";

}  // namespace

TEST(N8nImport, ImportsAndRoundTripsChurnFixture) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto project = node.create_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->input_count(), 0u);

    const std::string raw = read_raw(churn_fixture());
    ASSERT_FALSE(raw.empty());

    auto result = import_n8n_workflow_file(*project, churn_fixture(),
                                           "user:steve");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_TRUE(result.input);

    // Stored as an uploaded artifact, keeping the original filename + provenance.
    EXPECT_EQ(result.input->kind(), ProjectInputKind::Artifact);
    EXPECT_EQ(result.input->source(), ProjectInputSource::Upload);
    EXPECT_EQ(result.input->filename(), "churn_workflow.json");
    EXPECT_EQ(result.input->created_by(), "user:steve");
    EXPECT_EQ(result.input->size(), raw.size());

    // sha256 is computed at the boundary over the raw bytes.
    EXPECT_EQ(result.input->content_hash(), zetta::sha256_hex(raw));

    // The payload round-trips byte-for-byte for later re-compilation.
    ASSERT_TRUE(result.input->read_payload().has_value());
    EXPECT_EQ(*result.input->read_payload(), raw);

    EXPECT_EQ(project->input_count(), 1u);
    node.stop();
}

TEST(N8nImport, Sha256IsStableAcrossImports) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto project = node.create_project("churn");
    ASSERT_TRUE(project);

    const std::string raw = read_raw(churn_fixture());
    auto first = import_n8n_workflow(*project, raw, "churn_workflow.json");
    auto second = import_n8n_workflow(*project, raw, "churn_workflow.json");
    ASSERT_TRUE(first.ok()) << first.error;
    ASSERT_TRUE(second.ok()) << second.error;

    // Identical content -> identical hash, equal to sha256 of the raw bytes.
    EXPECT_EQ(first.input->content_hash(), second.input->content_hash());
    EXPECT_EQ(first.input->content_hash(), zetta::sha256_hex(raw));
    // Distinct records (duplicates allowed), same fingerprint.
    EXPECT_NE(first.input->id(), second.input->id());
    EXPECT_EQ(project->input_count(), 2u);
    node.stop();
}

TEST(N8nImport, AcceptsMinimalAndRealExports) {
    std::string error;
    EXPECT_TRUE(validate_n8n_workflow(kMinimalWorkflow, &error)) << error;
    EXPECT_TRUE(validate_n8n_workflow(read_raw(churn_fixture()), &error))
        << error;
}

TEST(N8nImport, RejectsInvalidJson) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto project = node.create_project("churn");
    ASSERT_TRUE(project);

    auto result = import_n8n_workflow(*project, "{not valid json",
                                      "broken.json");
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.input);
    EXPECT_NE(result.error.find("invalid JSON"), std::string::npos)
        << result.error;
    EXPECT_EQ(project->input_count(), 0u);  // nothing stored on rejection
    node.stop();
}

TEST(N8nImport, RejectsValidJsonThatIsNotAWorkflow) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto project = node.create_project("churn");
    ASSERT_TRUE(project);

    // Parses as JSON, but lacks the n8n structural signature.
    for (const char* not_a_workflow :
         {R"({"foo": 1})", R"([1, 2, 3])", R"("just a string")",
          R"({"nodes": "oops", "connections": {}})",
          R"({"nodes": [{"name": "X"}], "connections": {}})"}) {
        auto result = import_n8n_workflow(*project, not_a_workflow, "x.json");
        EXPECT_FALSE(result.ok()) << not_a_workflow;
        EXPECT_NE(result.error.find("not an n8n workflow"), std::string::npos)
            << result.error;
    }
    EXPECT_EQ(project->input_count(), 0u);
    node.stop();
}

TEST(N8nImport, MissingFileErrors) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto project = node.create_project("churn");
    ASSERT_TRUE(project);

    auto result =
        import_n8n_workflow_file(*project, tmp.path() / "nope.json");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(project->input_count(), 0u);
    node.stop();
}
