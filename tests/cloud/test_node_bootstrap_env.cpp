// Node bootstrap from provisioning env (#749): the env->flag mapping a
// burst/provisioned node relies on to auto-join a secure cluster with no manual
// flags. Exercised through a fake EnvReader so the process environment is never
// touched. See src/core/cloud/node_bootstrap_env.cpp and
// docs/plans/cloud-provisioning.md Gap A.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cloud/node_bootstrap_env.hpp"

using zetta::cloud::apply_bootstrap_env;
using zetta::cloud::BootstrapEnvApplied;
using zetta::cloud::EnvReader;
using zetta::cloud::NodeBootstrap;
using zetta::cloud::split_seed_peers;

namespace {

// An EnvReader backed by a fixed map, so the mapping is exercised without
// touching the process environment. A missing key reads as unset (nullptr),
// matching std::getenv.
EnvReader env_from(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* name) -> const char* {
        const auto found = vars.find(name);
        return found == vars.end() ? nullptr : found->second.c_str();
    };
}

// Well-formed join tokens (64 lowercase hex).
const std::string kToken(64, 'a');
const std::string kOtherToken(64, 'b');

// Write `contents` to `path` with `permissions`, the way a Secret volume or a
// provisioning unit lays the token file down.
void write_file(const std::filesystem::path& path, const std::string& contents,
                std::filesystem::perms permissions = std::filesystem::perms::owner_read |
                                                     std::filesystem::perms::owner_write) {
    std::ofstream(path, std::ios::binary) << contents;
    std::filesystem::permissions(path, permissions);
}

}  // namespace

// ---- split_seed_peers: one, comma-list, space-list, mixed, blanks -----------

TEST(NodeBootstrapEnv, SplitsSeedPeersOnCommasAndWhitespace) {
    EXPECT_EQ(split_seed_peers("10.0.0.1:7777"),
              (std::vector<std::string>{"10.0.0.1:7777"}));
    EXPECT_EQ(split_seed_peers("a:1,b:2,c:3"),
              (std::vector<std::string>{"a:1", "b:2", "c:3"}));
    EXPECT_EQ(split_seed_peers("a:1 b:2\tc:3"),
              (std::vector<std::string>{"a:1", "b:2", "c:3"}));
    EXPECT_EQ(split_seed_peers(" a:1 , , b:2 "),
              (std::vector<std::string>{"a:1", "b:2"}));
    EXPECT_TRUE(split_seed_peers("").empty());
    EXPECT_TRUE(split_seed_peers("   ,  ").empty());
}

// ---- env populates an otherwise-empty config (the provisioned-node path) -----

TEST(NodeBootstrapEnv, EnvFillsSeedsAndTokenWhenNoFlagsGiven) {
    NodeBootstrap config;              // nothing set on the CLI
    config.data_dir = "./zetta-data";  // the default

    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_SEED_PEER", "seed-a:7777, seed-b:7777"},
                          {"ZETTA_CLUSTER_TOKEN", kToken},
                          {"ZETTA_DATA_DIR", "/var/lib/zetta"}}));

    // The seeds and token land on the config exactly as --peer/--join-token would.
    EXPECT_EQ(config.seeds,
              (std::vector<std::string>{"seed-a:7777", "seed-b:7777"}));
    EXPECT_EQ(config.join_token, kToken);
    EXPECT_EQ(config.data_dir, "/var/lib/zetta");

    // ...and the applied record names each for the startup log line.
    EXPECT_EQ(applied.seeds,
              (std::vector<std::string>{"seed-a:7777", "seed-b:7777"}));
    EXPECT_TRUE(applied.join_token);
    EXPECT_EQ(applied.join_token_source, "ZETTA_CLUSTER_TOKEN");
    EXPECT_EQ(applied.data_dir, "/var/lib/zetta");
    EXPECT_TRUE(applied.any());
    EXPECT_TRUE(applied.error.empty());
}

// ---- explicit CLI flags override env (precedence: flag > env > default) ------

TEST(NodeBootstrapEnv, CliFlagsOverrideEnv) {
    NodeBootstrap config;
    config.seeds = {"cli-seed:7777"};  // --peer cli-seed:7777
    config.peer_from_cli = true;
    config.join_token = "clitoken";  // --join-token clitoken
    config.join_token_from_cli = true;
    config.data_dir = "/cli/dir";  // --data-dir /cli/dir
    config.data_dir_from_cli = true;

    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_SEED_PEER", "env-seed:7777"},
                          {"ZETTA_CLUSTER_TOKEN", "envtoken"},
                          {"ZETTA_DATA_DIR", "/env/dir"}}));

    // The CLI values are untouched; env contributed nothing.
    EXPECT_EQ(config.seeds, (std::vector<std::string>{"cli-seed:7777"}));
    EXPECT_EQ(config.join_token, "clitoken");
    EXPECT_EQ(config.data_dir, "/cli/dir");
    EXPECT_FALSE(applied.any());
}

// ---- no env set: config is unchanged, nothing applied ------------------------

TEST(NodeBootstrapEnv, NoEnvLeavesConfigUnchanged) {
    NodeBootstrap config;
    config.data_dir = "./zetta-data";
    const BootstrapEnvApplied applied =
        apply_bootstrap_env(config, env_from({}));
    EXPECT_TRUE(config.seeds.empty());
    EXPECT_TRUE(config.join_token.empty());
    EXPECT_EQ(config.data_dir, "./zetta-data");
    EXPECT_FALSE(applied.any());
}

// ---- blank env values are ignored (unset semantics) --------------------------

TEST(NodeBootstrapEnv, BlankEnvValuesAreIgnored) {
    NodeBootstrap config;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_SEED_PEER", "   "},
                          {"ZETTA_CLUSTER_TOKEN", ""}}));
    EXPECT_TRUE(config.seeds.empty());
    EXPECT_TRUE(config.join_token.empty());
    EXPECT_FALSE(applied.any());
}

// ---- #770: the token is trimmed, validated against its source, and a file is
// ---- preferred over the environment -----------------------------------------

TEST(NodeBootstrapEnv, WhitespaceOnlyTokenIsIgnoredNotAdopted) {
    NodeBootstrap config;
    const BootstrapEnvApplied applied =
        apply_bootstrap_env(config, env_from({{"ZETTA_CLUSTER_TOKEN", "   \n"}}));
    EXPECT_TRUE(config.join_token.empty());
    EXPECT_FALSE(applied.join_token);
    EXPECT_TRUE(applied.error.empty());
}

TEST(NodeBootstrapEnv, TokenIsTrimmed) {
    NodeBootstrap config;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_CLUSTER_TOKEN", "  " + kToken + "\n"}}));
    EXPECT_EQ(config.join_token, kToken);
    EXPECT_TRUE(applied.join_token);
    EXPECT_TRUE(applied.error.empty());
}

TEST(NodeBootstrapEnv, MalformedEnvTokenFailsNamingItsSource) {
    NodeBootstrap config;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_CLUSTER_TOKEN", "deadbeef"}}));
    EXPECT_FALSE(applied.join_token);
    EXPECT_TRUE(config.join_token.empty());
    EXPECT_NE(applied.error.find("ZETTA_CLUSTER_TOKEN"), std::string::npos);
    EXPECT_EQ(applied.error.find("--join-token"), std::string::npos);
    EXPECT_EQ(applied.error.find("deadbeef"), std::string::npos);  // never the value
}

TEST(NodeBootstrapEnv, TokenFileSuppliesTheTokenTrimmed) {
    zetta::test::TempDir dir;
    const auto path = dir.path() / "cluster-token";
    write_file(path, kToken + "\n");

    NodeBootstrap config;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_CLUSTER_TOKEN_FILE", path.string()}}));
    EXPECT_TRUE(applied.error.empty()) << applied.error;
    EXPECT_EQ(config.join_token, kToken);
    EXPECT_TRUE(applied.join_token);
    EXPECT_EQ(applied.join_token_source,
              "ZETTA_CLUSTER_TOKEN_FILE (" + path.string() + ")");
    EXPECT_TRUE(applied.warnings.empty());
}

TEST(NodeBootstrapEnv, TokenFileAndMatchingEnvAreAccepted) {
    // An operator moving from the variable to the file sets both for a while.
    zetta::test::TempDir dir;
    const auto path = dir.path() / "cluster-token";
    write_file(path, kToken);

    NodeBootstrap config;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_CLUSTER_TOKEN_FILE", path.string()},
                          {"ZETTA_CLUSTER_TOKEN", kToken}}));
    EXPECT_TRUE(applied.error.empty()) << applied.error;
    EXPECT_EQ(config.join_token, kToken);
    EXPECT_NE(applied.join_token_source.find("ZETTA_CLUSTER_TOKEN_FILE"),
              std::string::npos);
}

TEST(NodeBootstrapEnv, TokenFileAndDifferentEnvRefuseTheStart) {
    zetta::test::TempDir dir;
    const auto path = dir.path() / "cluster-token";
    write_file(path, kToken);

    NodeBootstrap config;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_CLUSTER_TOKEN_FILE", path.string()},
                          {"ZETTA_CLUSTER_TOKEN", kOtherToken}}));
    EXPECT_FALSE(applied.error.empty());
    EXPECT_FALSE(applied.join_token);
    EXPECT_TRUE(config.join_token.empty());
    EXPECT_EQ(applied.error.find(kToken), std::string::npos);
    EXPECT_EQ(applied.error.find(kOtherToken), std::string::npos);
}

TEST(NodeBootstrapEnv, MissingEmptyOrMalformedTokenFileFails) {
    zetta::test::TempDir dir;
    const auto missing = dir.path() / "absent";
    const auto empty = dir.path() / "empty";
    const auto malformed = dir.path() / "malformed";
    write_file(empty, " \n");
    write_file(malformed, "not-a-token\n");

    for (const auto& path : {missing, empty, malformed}) {
        NodeBootstrap config;
        const BootstrapEnvApplied applied = apply_bootstrap_env(
            config, env_from({{"ZETTA_CLUSTER_TOKEN_FILE", path.string()}}));
        EXPECT_FALSE(applied.error.empty()) << path;
        EXPECT_NE(applied.error.find(path.string()), std::string::npos) << applied.error;
        EXPECT_FALSE(applied.join_token);
        EXPECT_TRUE(config.join_token.empty());
    }
}

TEST(NodeBootstrapEnv, WorldReadableTokenFileWarns) {
    zetta::test::TempDir dir;
    const auto path = dir.path() / "cluster-token";
    write_file(path, kToken,
               std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                   std::filesystem::perms::group_read | std::filesystem::perms::others_read);

    NodeBootstrap config;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_CLUSTER_TOKEN_FILE", path.string()}}));
    EXPECT_TRUE(applied.error.empty()) << applied.error;
    EXPECT_EQ(config.join_token, kToken);
    ASSERT_EQ(applied.warnings.size(), 1u);
    EXPECT_NE(applied.warnings.front().find("every user"), std::string::npos);
}

TEST(NodeBootstrapEnv, CliTokenIgnoresTheTokenFile) {
    NodeBootstrap config;
    config.join_token = kToken;
    config.join_token_from_cli = true;
    const BootstrapEnvApplied applied = apply_bootstrap_env(
        config, env_from({{"ZETTA_CLUSTER_TOKEN_FILE", "/does/not/exist"}}));
    EXPECT_TRUE(applied.error.empty());
    EXPECT_EQ(config.join_token, kToken);
    EXPECT_FALSE(applied.join_token);
}

TEST(NodeBootstrapEnv, ScrubRemovesTheTokenFromTheProcessEnvironment) {
    ::setenv("ZETTA_CLUSTER_TOKEN", kToken.c_str(), 1);
    ::setenv("ZETTA_SEED_PEER", "seed:7777", 1);
    zetta::cloud::scrub_bootstrap_secrets();
    EXPECT_EQ(std::getenv("ZETTA_CLUSTER_TOKEN"), nullptr);
    // Only the secret goes; the seed is not a secret.
    ASSERT_NE(std::getenv("ZETTA_SEED_PEER"), nullptr);
    ::unsetenv("ZETTA_SEED_PEER");
}
