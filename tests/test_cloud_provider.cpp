#include "zetta/cloud_provider.hpp"

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
    }
}

TEST(CloudProviderCatalog, AwsNamesItsCredentialFiles) {
    const CloudProvider* aws = CloudProviderCatalog::builtin().find("aws");
    ASSERT_NE(aws, nullptr);
    EXPECT_EQ(aws->cli, "aws");
    EXPECT_TRUE(contains(aws->config_paths, "~/.aws/credentials"));
    EXPECT_TRUE(contains(aws->config_paths, "~/.aws/config"));
    // The inspect commands must be read-only — no provision/deploy/teardown verbs.
    for (const std::string& command : aws->inspect_commands) {
        EXPECT_EQ(command.find("delete"), std::string::npos) << command;
        EXPECT_EQ(command.find("create"), std::string::npos) << command;
        EXPECT_EQ(command.find("terminate"), std::string::npos) << command;
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
