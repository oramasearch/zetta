// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/export_path.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

// The export root (#1397, #1399): a file a caller has the node write
// (`generate_dataset`'s `path`, `submit_feedback`'s `export_path`) is created new under
// `<data_dir>/exports`, with no absolute path, no `..`, no symbolic link and never over
// an existing file, so a caller cannot truncate the node's security files or state.

namespace {

namespace fs = std::filesystem;

using zetta::Node;
using zetta::NodeConfig;
using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::BackgroundActions;
using zetta::api::Registry;
using zetta::data::write_export_file;
using zetta::test::TempDir;

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

constexpr const char* kSentinel = "{\"join_token\":\"sentinel-not-a-real-token\"}\n";

// --- the writer ------------------------------------------------------------------

struct ExportPathWriter : ::testing::Test {
    TempDir dir;
    fs::path root = dir.path() / "exports";
    fs::path sentinel = dir.path() / "security" / "cluster-secret.json";

    void SetUp() override { write_file(sentinel, kSentinel); }
};

TEST_F(ExportPathWriter, WritesANewFileUnderTheRootCreatingDirectories) {
    const auto written = write_export_file(root, "corpora/2026/base.jsonl", "line\n");
    ASSERT_TRUE(written.ok()) << written.error;
    EXPECT_EQ(written.relative, "corpora/2026/base.jsonl");
    EXPECT_EQ(written.path, root / "corpora/2026/base.jsonl");
    EXPECT_EQ(read_file(root / "corpora/2026/base.jsonl"), "line\n");
    // Only the owner can read what was exported.
    const auto permissions = fs::status(root / "corpora/2026/base.jsonl").permissions();
    EXPECT_EQ(permissions & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);

    const auto dotted = write_export_file(root, "./feedback.json", "{}");
    ASSERT_TRUE(dotted.ok()) << dotted.error;
    EXPECT_EQ(dotted.relative, "feedback.json");
}

TEST_F(ExportPathWriter, AnAbsoluteOrEscapingPathIsRefusedAndTheSentinelSurvives) {
    for (const std::string& path :
         {sentinel.string(), (root / "inside.json").string(), std::string("../security/cluster-secret.json"),
          std::string("a/../../security/cluster-secret.json"), std::string("~/x.json"),
          std::string("a\\b.json"), std::string(""), std::string("."), std::string("a/..")}) {
        const auto written = write_export_file(root, path, "overwritten");
        EXPECT_FALSE(written.ok()) << path;
    }
    EXPECT_EQ(read_file(sentinel), kSentinel);
    EXPECT_FALSE(fs::exists(root / "inside.json"));
}

TEST_F(ExportPathWriter, AnExistingFileIsNeverOverwritten) {
    ASSERT_TRUE(write_export_file(root, "report.json", "first").ok());
    const auto again = write_export_file(root, "report.json", "second");
    EXPECT_FALSE(again.ok());
    EXPECT_NE(again.error.find("already exists"), std::string::npos) << again.error;
    EXPECT_EQ(read_file(root / "report.json"), "first");
}

TEST_F(ExportPathWriter, ASymbolicLinkIsNeverFollowed) {
    fs::create_directories(root);
    // A link at the end, to the sentinel.
    fs::create_symlink(sentinel, root / "secret.json");
    // A dangling link at the end, pointing outside: O_CREAT would otherwise create it.
    fs::create_symlink(dir.path() / "planted.json", root / "dangling.json");
    // A link on the way, to the security directory.
    fs::create_symlink(sentinel.parent_path(), root / "security");
    for (const char* path : {"secret.json", "dangling.json", "security/cluster-secret.json",
                             "security/new.json"}) {
        const auto written = write_export_file(root, path, "overwritten");
        EXPECT_FALSE(written.ok()) << path;
        EXPECT_NE(written.error.find("symbolic link"), std::string::npos)
            << path << ": " << written.error;
    }
    EXPECT_EQ(read_file(sentinel), kSentinel);
    EXPECT_FALSE(fs::exists(dir.path() / "planted.json"));
    EXPECT_FALSE(fs::exists(sentinel.parent_path() / "new.json"));
}

TEST_F(ExportPathWriter, TheRootItselfMayNotBeALink) {
    fs::create_symlink(sentinel.parent_path(), root);  // exports -> security
    const auto written = write_export_file(root, "cluster-secret.json", "overwritten");
    EXPECT_FALSE(written.ok());
    EXPECT_EQ(read_file(sentinel), kSentinel);
}

// --- through the API -------------------------------------------------------------

struct ExportPathApi : ::testing::Test {
    TempDir dir;
    Node node{NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    BackgroundActions background;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    fs::path sentinel = dir.path() / "security" / "cluster-secret.json";
    std::string original;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        if (!fs::exists(sentinel)) write_file(sentinel, kSentinel);
        original = read_file(sentinel);
    }
    void TearDown() override {
        background.prune();
        node.stop();
    }

    ApiResult call(const std::string& name, const nlohmann::json& input) {
        ApiContext context{node, nullptr, {}, trace, &node.span_recorder(), &background};
        return registry.call(name, input, context);
    }

    // Every spelling a caller might use to reach the node's security file.
    std::vector<std::string> escapes() {
        fs::create_directories(zetta::data::export_root(node));
        std::error_code ignored;
        fs::create_symlink(sentinel, zetta::data::export_root(node) / "link.json", ignored);
        return {sentinel.string(), "../security/cluster-secret.json",
                "x/../../security/cluster-secret.json", "link.json"};
    }
};

// #1397: the generated corpus cannot be written over a node file.
TEST_F(ExportPathApi, GenerateDatasetCannotTruncateANodeFile) {
    for (const std::string& path : escapes()) {
        const ApiResult generated = call("generate_dataset", {{"kind", "base-agent"}, {"path", path}});
        EXPECT_FALSE(generated.ok) << path;
        EXPECT_EQ(generated.error.code, "invalid_input") << path;
        EXPECT_EQ(generated.error.path, "/path") << path;
    }
    EXPECT_EQ(read_file(sentinel), original);
}

TEST_F(ExportPathApi, GenerateDatasetWritesItsCorpusUnderTheExportRoot) {
    const ApiResult generated =
        call("generate_dataset", {{"kind", "base-agent"}, {"path", "corpora/base.jsonl"}});
    ASSERT_TRUE(generated.ok) << generated.error.message;
    EXPECT_EQ(generated.value.at("path"), "corpora/base.jsonl");
    const std::string corpus = read_file(zetta::data::export_root(node) / "corpora/base.jsonl");
    EXPECT_FALSE(corpus.empty());
    // A second run with the same name does not overwrite the first.
    const ApiResult again =
        call("generate_dataset", {{"kind", "base-agent"}, {"path", "corpora/base.jsonl"}});
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(again.error.path, "/path");
}

// #1399: the feedback bundle cannot be written over a node file.
TEST_F(ExportPathApi, SubmitFeedbackCannotOverwriteANodeFile) {
    for (const std::string& path : escapes()) {
        const ApiResult submitted =
            call("submit_feedback", {{"description", "export test"}, {"export_path", path}});
        EXPECT_FALSE(submitted.ok) << path;
        EXPECT_EQ(submitted.error.code, "invalid_input") << path;
        EXPECT_EQ(submitted.error.path, "/export_path") << path;
    }
    EXPECT_EQ(read_file(sentinel), original);
}

TEST_F(ExportPathApi, SubmitFeedbackExportsUnderTheExportRoot) {
    const ApiResult submitted =
        call("submit_feedback", {{"description", "export test"}, {"export_path", "feedback/one.json"}});
    ASSERT_TRUE(submitted.ok) << submitted.error.message;
    EXPECT_EQ(submitted.value.at("exported_to"), "feedback/one.json");
    const nlohmann::json bundle = nlohmann::json::parse(
        read_file(zetta::data::export_root(node) / "feedback/one.json"), nullptr, false);
    ASSERT_TRUE(bundle.is_object());
    EXPECT_EQ(bundle.value("description", std::string()), "export test");
}

}  // namespace
