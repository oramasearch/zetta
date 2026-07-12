// Node bootstrap from provisioning env (#749): the env->flag mapping a
// burst/provisioned node relies on to auto-join a secure cluster with no manual
// flags. Exercised through a fake EnvReader so the process environment is never
// touched. See src/core/cloud/node_bootstrap_env.cpp and
// docs/plans/cloud-provisioning.md Gap A.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

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
                          {"ZETTA_CLUSTER_TOKEN", "deadbeef"},
                          {"ZETTA_DATA_DIR", "/var/lib/zetta"}}));

    // The seeds and token land on the config exactly as --peer/--join-token would.
    EXPECT_EQ(config.seeds,
              (std::vector<std::string>{"seed-a:7777", "seed-b:7777"}));
    EXPECT_EQ(config.join_token, "deadbeef");
    EXPECT_EQ(config.data_dir, "/var/lib/zetta");

    // ...and the applied record names each for the startup log line.
    EXPECT_EQ(applied.seeds,
              (std::vector<std::string>{"seed-a:7777", "seed-b:7777"}));
    EXPECT_TRUE(applied.join_token);
    EXPECT_EQ(applied.data_dir, "/var/lib/zetta");
    EXPECT_TRUE(applied.any());
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
