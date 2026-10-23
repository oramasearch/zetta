#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/import_path.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

// The import root (#1391, #1433): every path a caller names resolves under
// `<data_dir>/imports`, with no absolute path, no `..` and no symbolic link, so the
// node's security files are never readable as data.

namespace {

namespace fs = std::filesystem;

using zetta::Node;
using zetta::NodeConfig;
using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::BackgroundActions;
using zetta::api::Registry;
using zetta::data::ImportEntry;
using zetta::data::resolve_import_path;
using zetta::test::TempDir;

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// --- the resolver --------------------------------------------------------------

struct ImportPathResolver : ::testing::Test {
    TempDir dir;
    fs::path root = dir.path() / "imports";
    fs::path outside = dir.path() / "security" / "cluster-secret.json";

    void SetUp() override {
        write_file(root / "accounts.jsonl", "{\"id\":1}\n");
        write_file(root / "churn" / "dataset.json", "{}");
        write_file(outside, "{\"join_token\":\"not-a-real-token\"}");
    }
};

TEST_F(ImportPathResolver, AFileUnderTheRootResolves) {
    const auto staged = resolve_import_path(root, "accounts.jsonl", ImportEntry::File);
    ASSERT_TRUE(staged.ok()) << staged.error;
    EXPECT_EQ(staged.path, fs::canonical(root / "accounts.jsonl"));

    const auto dotted = resolve_import_path(root, "./churn", ImportEntry::Directory);
    ASSERT_TRUE(dotted.ok()) << dotted.error;
    EXPECT_EQ(dotted.path, fs::canonical(root / "churn"));
}

TEST_F(ImportPathResolver, AnAbsolutePathIsRefusedEvenUnderTheRoot) {
    for (const fs::path& path : {outside, fs::canonical(root / "accounts.jsonl")}) {
        const auto staged = resolve_import_path(root, path.string(), ImportEntry::File);
        EXPECT_FALSE(staged.ok()) << path;
        EXPECT_NE(staged.error.find("absolute"), std::string::npos) << staged.error;
    }
}

TEST_F(ImportPathResolver, DotDotIsRefusedWherever) {
    for (const char* path : {"../security/cluster-secret.json", "churn/../../security/cluster-secret.json",
                             "churn/..", ".."}) {
        const auto staged = resolve_import_path(root, path, ImportEntry::File);
        EXPECT_FALSE(staged.ok()) << path;
        EXPECT_NE(staged.error.find("'..'"), std::string::npos) << staged.error;
    }
}

TEST_F(ImportPathResolver, TildeBackslashAndEmptyAreRefused) {
    EXPECT_FALSE(resolve_import_path(root, "~/accounts.jsonl", ImportEntry::File).ok());
    EXPECT_FALSE(resolve_import_path(root, "churn\\dataset.json", ImportEntry::File).ok());
    EXPECT_FALSE(resolve_import_path(root, "", ImportEntry::File).ok());
    EXPECT_FALSE(resolve_import_path(root, ".", ImportEntry::Directory).ok());
}

TEST_F(ImportPathResolver, ASymbolicLinkToTheSecurityFileIsRefused) {
    fs::create_symlink(outside, root / "secret.json");
    const auto staged = resolve_import_path(root, "secret.json", ImportEntry::File);
    EXPECT_FALSE(staged.ok());
    EXPECT_NE(staged.error.find("symbolic link"), std::string::npos) << staged.error;
}

TEST_F(ImportPathResolver, ASymbolicLinkOnTheWayIsRefused) {
    fs::create_directory_symlink(outside.parent_path(), root / "keys");
    const auto file = resolve_import_path(root, "keys/cluster-secret.json", ImportEntry::File);
    EXPECT_FALSE(file.ok());
    EXPECT_NE(file.error.find("symbolic link"), std::string::npos) << file.error;
    const auto directory = resolve_import_path(root, "keys", ImportEntry::Directory);
    EXPECT_FALSE(directory.ok());
    EXPECT_NE(directory.error.find("symbolic link"), std::string::npos) << directory.error;
}

TEST_F(ImportPathResolver, TheEntryMustBeTheKindAskedFor) {
    EXPECT_FALSE(resolve_import_path(root, "churn", ImportEntry::File).ok());
    EXPECT_FALSE(resolve_import_path(root, "accounts.jsonl", ImportEntry::Directory).ok());
    const auto missing = resolve_import_path(root, "nope.jsonl", ImportEntry::File);
    EXPECT_FALSE(missing.ok());
    EXPECT_NE(missing.error.find("no such file"), std::string::npos) << missing.error;
}

TEST(ImportPathRoot, AMissingRootRefusesEverything) {
    TempDir dir;
    const auto staged = resolve_import_path(dir.path() / "imports", "a.jsonl", ImportEntry::File);
    EXPECT_FALSE(staged.ok());
    EXPECT_NE(staged.error.find("does not exist"), std::string::npos) << staged.error;
}

// --- through the API -----------------------------------------------------------

struct ImportPathApi : ::testing::Test {
    TempDir dir;
    Node node{NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    BackgroundActions background;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    fs::path secret = dir.path() / "security" / "cluster-secret.json";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        // A dummy cluster secret in the node's real security directory: a JSON
        // object the decoder would happily turn into a row.
        if (!fs::exists(secret)) write_file(secret, "{\"join_token\":\"dummy-join-token\"}\n");
    }
    void TearDown() override {
        background.prune();
        node.stop();
    }

    ApiResult call(const std::string& name, const nlohmann::json& input) {
        ApiContext context{node, nullptr, {}, trace, &node.span_recorder(), &background};
        return registry.call(name, input, context);
    }
};

// LSR-071 (#1391): the security file is not importable by any spelling.
TEST_F(ImportPathApi, AddDatasetCannotImportTheClusterSecret) {
    write_file(zetta::data::import_root(node) / "placeholder.jsonl", "{\"id\":1}\n");
    fs::create_symlink(secret, zetta::data::import_root(node) / "secret.json");
    for (const std::string& path :
         {secret.string(), std::string("../security/cluster-secret.json"),
          std::string("secret.json"), std::string("~/../security/cluster-secret.json")}) {
        const ApiResult added = call("add_dataset", {{"path", path}, {"table_name", "leak"}});
        EXPECT_FALSE(added.ok) << path;
        EXPECT_EQ(added.error.code, "invalid_input") << path;
        EXPECT_EQ(added.error.path, "/path") << path;
    }
    const auto datasets = node.find_database("datasets");
    EXPECT_TRUE(!datasets || !datasets->find_table("leak"));
}

TEST_F(ImportPathApi, AddDatasetLoadsAFileStagedUnderTheImportRoot) {
    write_file(zetta::data::import_root(node) / "batch" / "accounts.jsonl",
               "{\"id\":1,\"churned\":0}\n{\"id\":2,\"churned\":1}\n");
    const ApiResult added =
        call("add_dataset", {{"path", "batch/accounts.jsonl"}, {"partition_count", 1},
                             {"replication_factor", 1}});
    ASSERT_TRUE(added.ok) << added.error.message;
    EXPECT_EQ(added.value.at("table"), "accounts");
    EXPECT_EQ(added.value.at("rows"), 2);
}

TEST_F(ImportPathApi, TestsuiteRunRefusesACustomDirectoryOutsideTheImportRoot) {
    for (const std::string& path :
         {dir.path().string(), std::string("../security"), std::string("/etc")}) {
        const ApiResult run = call("testsuite_run", {{"case", "pipeline.uc1_end_to_end"},
                                                     {"custom_data_path", path}});
        EXPECT_FALSE(run.ok) << path;
        EXPECT_EQ(run.error.code, "invalid_input") << path;
        EXPECT_EQ(run.error.path, "/custom_data_path") << path;
    }
}

}  // namespace
