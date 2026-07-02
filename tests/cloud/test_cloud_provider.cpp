#include "zetta/cloud/cloud_provider.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace zetta;

namespace {

// Does `haystack` contain `needle`?
bool contains(const std::vector<std::string>& haystack,
              const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

TEST(CloudProviderCatalog, BuiltinCoversEveryBackend) {
    const CloudProviderCatalog& catalog = CloudProviderCatalog::builtin();
    for (const char* id :
         {"local", "bare-metal", "kubernetes", "aws", "gcp", "azure", "oci"}) {
        EXPECT_NE(catalog.find(id), nullptr) << "missing provider: " << id;
    }
    EXPECT_EQ(catalog.size(), 7u);
    EXPECT_FALSE(catalog.empty());
}

TEST(CloudProviderCatalog, FindReturnsNullForUnknownId) {
    EXPECT_EQ(CloudProviderCatalog::builtin().find("nope"), nullptr);
    EXPECT_EQ(CloudProviderCatalog::builtin().find(""), nullptr);
}

TEST(CloudProviderCatalog, LocalReadsNothingAndNeedsNoConsent) {
    const CloudProvider* local = CloudProviderCatalog::builtin().find("local");
    ASSERT_NE(local, nullptr);
    EXPECT_FALSE(local->requires_consent());
    EXPECT_FALSE(local->runnable());
    EXPECT_TRUE(local->config_paths.empty());
    EXPECT_TRUE(local->inspect_commands.empty());
    EXPECT_TRUE(local->cli.empty());
}

TEST(CloudProviderCatalog, EveryNonLocalProviderRequiresConsent) {
    for (const CloudProvider& provider :
         CloudProviderCatalog::builtin().providers()) {
        if (provider.id == "local") continue;
        EXPECT_TRUE(provider.requires_consent())
            << provider.id << " should require consent";
        // A provider that needs consent must name what it would touch, so the
        // gate can show it.
        EXPECT_FALSE(provider.config_paths.empty()) << provider.id;
        EXPECT_FALSE(provider.inspect_commands.empty()) << provider.id;
        EXPECT_FALSE(provider.cli.empty()) << provider.id;
        for (const InspectCommand& command : provider.inspect_commands) {
            EXPECT_FALSE(command.display.empty()) << provider.id;
        }
    }
}

TEST(CloudProviderCatalog, RunnableProvidersHaveArgvAndBareMetalDoesNot) {
    const CloudProviderCatalog& catalog = CloudProviderCatalog::builtin();
    // The CLI providers are runnable (their inspect commands carry a real argv).
    for (const char* id : {"kubernetes", "aws", "gcp", "azure", "oci"}) {
        const CloudProvider* provider = catalog.find(id);
        ASSERT_NE(provider, nullptr) << id;
        EXPECT_TRUE(provider->runnable()) << id << " should be runnable";
    }
    // bare-metal is described (requires consent) but not yet runnable — its
    // commands need a concrete host, so they carry no argv.
    const CloudProvider* bare_metal = catalog.find("bare-metal");
    ASSERT_NE(bare_metal, nullptr);
    EXPECT_TRUE(bare_metal->requires_consent());
    EXPECT_FALSE(bare_metal->runnable());
    for (const InspectCommand& command : bare_metal->inspect_commands) {
        EXPECT_TRUE(command.argv.empty()) << command.display;
    }
}

TEST(CloudProviderCatalog, AwsNamesItsCredentialFiles) {
    const CloudProvider* aws = CloudProviderCatalog::builtin().find("aws");
    ASSERT_NE(aws, nullptr);
    EXPECT_EQ(aws->cli, "aws");
    EXPECT_TRUE(contains(aws->config_paths, "~/.aws/credentials"));
    EXPECT_TRUE(contains(aws->config_paths, "~/.aws/config"));
    // The inspect commands must be read-only — no provision/deploy/teardown verbs,
    // in either the display string or the argv that actually runs.
    for (const InspectCommand& command : aws->inspect_commands) {
        EXPECT_EQ(command.display.find("delete"), std::string::npos)
            << command.display;
        EXPECT_EQ(command.display.find("create"), std::string::npos)
            << command.display;
        EXPECT_EQ(command.display.find("terminate"), std::string::npos)
            << command.display;
        ASSERT_FALSE(command.argv.empty()) << command.display;
        EXPECT_EQ(command.argv.front(), "aws");
    }
}

TEST(CloudProviderCatalog, KubernetesNamesItsKubeconfig) {
    const CloudProvider* k8s = CloudProviderCatalog::builtin().find("kubernetes");
    ASSERT_NE(k8s, nullptr);
    EXPECT_EQ(k8s->cli, "kubectl");
    EXPECT_TRUE(contains(k8s->config_paths, "~/.kube/config"));
}

TEST(CloudProviderCatalog, ProviderIdsAreUnique) {
    std::vector<std::string> ids;
    for (const CloudProvider& provider :
         CloudProviderCatalog::builtin().providers()) {
        ids.push_back(provider.id);
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end())
        << "duplicate provider id in the catalog";
}
