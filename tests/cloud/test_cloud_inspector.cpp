#include "zetta/cloud/cloud_inspector.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "zetta/cloud/cloud_provider.hpp"
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
// the same way a missing CLI would. No real process is ever spawned.
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

const zetta::cloud::CloudProvider& provider(const char* id) {
    const zetta::cloud::CloudProvider* found = zetta::cloud::CloudProviderCatalog::builtin().find(id);
    EXPECT_NE(found, nullptr) << id;
    return *found;
}

}  // namespace

TEST(CloudInspector, AwsSuccessExtractsIdentityAndRegions) {
    std::map<std::string, zetta::cloud::CommandResult> outputs;
    outputs["get-caller-identity"] = ok_output(
        R"({"UserId":"AIDAX","Account":"123456789012",)"
        R"("Arn":"arn:aws:iam::123456789012:user/dev"})");
    outputs["describe-regions"] = ok_output(
        R"({"Regions":[{"RegionName":"us-east-1"},{"RegionName":"eu-west-1"}]})");

    zetta::cloud::CloudInspector inspector(fake_runner(outputs));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("aws"));

    EXPECT_TRUE(report.ran);
    EXPECT_TRUE(report.reachable);
    EXPECT_TRUE(report.authenticated);
    EXPECT_NE(report.identity.find("123456789012"), std::string::npos);
    ASSERT_EQ(report.locations.size(), 2u);
    EXPECT_EQ(report.locations.front(), "us-east-1");
    EXPECT_EQ(report.commands.size(), 2u);
    for (const zetta::cloud::CloudInspectCommand& command : report.commands) {
        EXPECT_TRUE(command.ok) << command.display;
        EXPECT_TRUE(command.error.empty()) << command.error;
    }
    EXPECT_FALSE(report.checked_at.empty());
}

TEST(CloudInspector, MissingCliIsReportedNotReachable) {
    // Every command "fails to run" — the CLI isn't installed.
    zetta::cloud::CloudInspector inspector(fake_runner({}));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("aws"));

    EXPECT_TRUE(report.ran);
    EXPECT_FALSE(report.reachable);
    EXPECT_FALSE(report.authenticated);
    ASSERT_FALSE(report.commands.empty());
    for (const zetta::cloud::CloudInspectCommand& command : report.commands) {
        EXPECT_FALSE(command.ok);
        EXPECT_EQ(command.exit_code, 127);
        EXPECT_NE(command.error.find("not found"), std::string::npos);
    }
    EXPECT_NE(report.detail.find("could not reach"), std::string::npos);
}

TEST(CloudInspector, MalformedJsonDegradesGracefully) {
    std::map<std::string, zetta::cloud::CommandResult> outputs;
    outputs["get-caller-identity"] = ok_output("this is not json");
    // Wrong shape: schema requires a Regions array of objects with RegionName.
    outputs["describe-regions"] = ok_output(R"({"Regions":"oops"})");

    zetta::cloud::CloudInspector inspector(fake_runner(outputs));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("aws"));

    // The commands "ran" (exit 0) so the provider is reachable, but neither
    // parsed, so no identity/regions were extracted and the failures are recorded.
    EXPECT_TRUE(report.reachable);
    EXPECT_FALSE(report.authenticated);
    EXPECT_TRUE(report.identity.empty());
    EXPECT_TRUE(report.locations.empty());
    ASSERT_EQ(report.commands.size(), 2u);
    EXPECT_NE(report.commands[0].error.find("not valid JSON"), std::string::npos);
    EXPECT_NE(report.commands[1].error.find("unexpected output shape"),
              std::string::npos);
}

TEST(CloudInspector, GcpExtractsAccountAndRegions) {
    std::map<std::string, zetta::cloud::CommandResult> outputs;
    outputs["config list"] =
        ok_output(R"({"core":{"account":"dev@example.com","project":"proj-1"}})");
    outputs["regions list"] =
        ok_output(R"([{"name":"us-central1"},{"name":"europe-west1"}])");

    zetta::cloud::CloudInspector inspector(fake_runner(outputs));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("gcp"));

    EXPECT_TRUE(report.authenticated);
    EXPECT_NE(report.identity.find("dev@example.com"), std::string::npos);
    EXPECT_NE(report.identity.find("proj-1"), std::string::npos);
    EXPECT_EQ(report.locations.size(), 2u);
}

TEST(CloudInspector, AzureExtractsUserAndLocations) {
    std::map<std::string, zetta::cloud::CommandResult> outputs;
    outputs["account show"] = ok_output(
        R"({"id":"sub-123","name":"Pay-As-You-Go","user":{"name":"dev@contoso.com"}})");
    outputs["list-locations"] = ok_output(
        R"([{"name":"eastus","displayName":"East US"},)"
        R"({"name":"westeurope","displayName":"West Europe"}])");

    zetta::cloud::CloudInspector inspector(fake_runner(outputs));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("azure"));

    EXPECT_TRUE(report.authenticated);
    EXPECT_NE(report.identity.find("dev@contoso.com"), std::string::npos);
    ASSERT_EQ(report.locations.size(), 2u);
    EXPECT_EQ(report.locations.front(), "East US");
}

TEST(CloudInspector, OciExtractsNamespaceAndRegions) {
    std::map<std::string, zetta::cloud::CommandResult> outputs;
    outputs["os ns get"] = ok_output(R"({"data":"mytenancyns"})");
    outputs["region list"] = ok_output(
        R"({"data":[{"key":"IAD","name":"us-ashburn-1"},)"
        R"({"key":"PHX","name":"us-phoenix-1"}]})");

    zetta::cloud::CloudInspector inspector(fake_runner(outputs));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("oci"));

    EXPECT_TRUE(report.authenticated);
    EXPECT_NE(report.identity.find("mytenancyns"), std::string::npos);
    EXPECT_EQ(report.locations.size(), 2u);
}

TEST(CloudInspector, KubernetesExtractsContextAndNodes) {
    std::map<std::string, zetta::cloud::CommandResult> outputs;
    outputs["config view"] = ok_output(
        R"({"current-context":"prod","clusters":[{"name":"prod",)"
        R"("cluster":{"server":"https://k8s.example.com"}}]})");
    outputs["get nodes"] = ok_output(
        R"({"items":[{"metadata":{"name":"node-a"}},)"
        R"({"metadata":{"name":"node-b"}}]})");

    zetta::cloud::CloudInspector inspector(fake_runner(outputs));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("kubernetes"));

    EXPECT_TRUE(report.authenticated);
    EXPECT_NE(report.identity.find("prod"), std::string::npos);
    ASSERT_EQ(report.resources.size(), 2u);
    EXPECT_EQ(report.resources.front(), "node-a");
}

TEST(CloudInspector, BareMetalIsNotRunnable) {
    // bare-metal is described but has no runnable argv (needs a host). The
    // inspector must NOT run anything and must report it honestly.
    bool runner_called = false;
    zetta::cloud::CommandRunner spy = [&runner_called](const std::vector<std::string>&) {
        runner_called = true;
        return zetta::cloud::CommandResult{};
    };
    zetta::cloud::CloudInspector inspector(spy);
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("bare-metal"));

    EXPECT_FALSE(runner_called);
    EXPECT_FALSE(report.ran);
    EXPECT_TRUE(report.commands.empty());
    EXPECT_FALSE(report.detail.empty());
}

TEST(CloudInspector, ReportRoundTripsThroughItsJsonBoundary) {
    std::map<std::string, zetta::cloud::CommandResult> outputs;
    outputs["get-caller-identity"] =
        ok_output(R"({"Account":"1","Arn":"arn:aws:iam::1:user/x"})");
    outputs["describe-regions"] =
        ok_output(R"({"Regions":[{"RegionName":"us-east-1"}]})");
    zetta::cloud::CloudInspector inspector(fake_runner(outputs));
    const zetta::cloud::CloudInspectReport report = inspector.inspect(provider("aws"));

    const nlohmann::json document = to_json(report);
    std::string error;
    EXPECT_TRUE(zetta::cloud::validate_cloud_inspect_report(document, &error)) << error;

    const std::optional<zetta::cloud::CloudInspectReport> parsed =
        zetta::cloud::parse_cloud_inspect_report(document, &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->provider_id, "aws");
    EXPECT_EQ(parsed->authenticated, report.authenticated);
    EXPECT_EQ(parsed->locations, report.locations);
    EXPECT_EQ(parsed->commands.size(), report.commands.size());
}

TEST(CloudInspector, MalformedReportFailsValidation) {
    // Missing the required provider_id / boolean fields.
    const nlohmann::json bad = {{"provider_name", "AWS"}};
    std::string error;
    EXPECT_FALSE(zetta::cloud::validate_cloud_inspect_report(bad, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(zetta::cloud::parse_cloud_inspect_report(bad).has_value());
}
