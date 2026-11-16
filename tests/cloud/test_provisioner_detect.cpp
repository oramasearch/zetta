// SPDX-License-Identifier: MIT

#include "zetta/cloud/provisioner_detect.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/command_runner.hpp"

using namespace zetta;

namespace {

zetta::cloud::CommandResult ok_output(std::string text) {
    zetta::cloud::CommandResult result;
    result.exit_code = 0;
    result.stdout_text = std::move(text);
    return result;
}

// A fake CommandRunner that returns canned output for the command whose joined
// argv contains a known signature; any other command "fails to run" (exit 127),
// like a missing CLI. No real process is ever spawned, so this never touches a
// real cloud — the same fake pattern test_cloud_inspector.cpp uses.
zetta::cloud::CommandRunner fake_runner(std::map<std::string, zetta::cloud::CommandResult> by_signature) {
    return [by_signature](const std::vector<std::string>& argv) -> zetta::cloud::CommandResult {
        std::string joined;
        for (const std::string& argument : argv) {
            joined += argument;
            joined += ' ';
        }
        for (const auto& [signature, result] : by_signature) {
            if (joined.find(signature) != std::string::npos) return result;
        }
        zetta::cloud::CommandResult missing;
        missing.exit_code = 127;  // command not found
        return missing;
    };
}

// `az account show` logged in, plus the operator's configured az defaults.
TEST(ProvisionerDetect, AzureDetectedFromLoggedInCli) {
    const zetta::cloud::CommandRunner run = fake_runner({
        {"account show",
         ok_output(R"({"id":"sub-123","tenantId":"tenant-abc","name":"Pay-As-You-Go"})")},
        {"defaults.group", ok_output(R"({"name":"group","value":"my-rg"})")},
        {"defaults.location", ok_output(R"({"name":"location","value":"westus2"})")},
    });

    const std::optional<zetta::cloud::ProvisionerConfig> detected = detect_azure_provisioner(run);
    ASSERT_TRUE(detected.has_value());
    EXPECT_EQ(detected->target, "azure");
    EXPECT_EQ(detected->cli, "az");

    // The detected context round-trips through the provider's own validated config.
    const std::optional<zetta::cloud::AzureConfig> config = zetta::cloud::AzureConfig::from_json(detected->config);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->subscription_id, "sub-123");
    EXPECT_EQ(config->tenant_id, "tenant-abc");
    EXPECT_EQ(config->resource_group, "my-rg");
    EXPECT_EQ(config->location, "westus2");
}

// With no az defaults set, detection still yields a valid config (so the
// Add-nodes button lights up), falling back to a stable burst group + region.
TEST(ProvisionerDetect, AzureFallsBackWhenNoDefaults) {
    const zetta::cloud::CommandRunner run = fake_runner({
        {"account show", ok_output(R"({"id":"sub-xyz"})")},
        // No "defaults.group" / "defaults.location" entries → az returns exit 127.
    });

    const std::optional<zetta::cloud::ProvisionerConfig> detected = detect_azure_provisioner(run);
    ASSERT_TRUE(detected.has_value());
    const std::optional<zetta::cloud::AzureConfig> config = zetta::cloud::AzureConfig::from_json(detected->config);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->subscription_id, "sub-xyz");
    EXPECT_FALSE(config->resource_group.empty());
    EXPECT_FALSE(config->location.empty());
}

// `az` absent or not logged in → nothing detected, no error, no throw.
TEST(ProvisionerDetect, AzureNotDetectedWhenCliMissing) {
    const zetta::cloud::CommandRunner run = fake_runner({});  // every command exits 127
    EXPECT_FALSE(detect_azure_provisioner(run).has_value());

    // The aggregate detector returns an empty set rather than failing.
    const std::vector<zetta::cloud::ProvisionerConfig> all = detect_provisioner_configs(run);
    EXPECT_TRUE(all.empty());
}

// Malformed CLI output (not the expected JSON) is rejected at the boundary.
TEST(ProvisionerDetect, AzureRejectsMalformedOutput) {
    const zetta::cloud::CommandRunner run = fake_runner({
        {"account show", ok_output("not json")},
    });
    EXPECT_FALSE(detect_azure_provisioner(run).has_value());
}

}  // namespace
