// A provisioned burst node is given the cluster's identity root (#1343).
//
// Burst nodes got ZETTA_SEED_PEER and ZETTA_CLUSTER_TOKEN but not
// ZETTA_IDENTITY_ROOT, so each made its own root and honored none of the
// cluster's accounts. The root's PUBLIC key now rides beside the seed peer on
// every control plane: an env var (Kubernetes, bare metal), a propagating tag
// (AWS), and the boot script's environment file (Azure, GCP, OCI). These tests
// pin each one, and that an empty root adds nothing.
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "zetta/cloud/aws_cli_control_plane.hpp"
#include "zetta/cloud/aws_provisioner.hpp"
#include "zetta/cloud/bare_metal_cli_control_plane.hpp"
#include "zetta/cloud/bare_metal_provisioner.hpp"
#include "zetta/cloud/command_runner.hpp"
#include "zetta/cloud/k8s_cli_control_plane.hpp"
#include "zetta/cloud/k8s_provisioner.hpp"
#include "zetta/cloud/node_bootstrap_script.hpp"
#include "zetta/cloud/provisioner.hpp"

using zetta::cloud::AwsCliControlPlane;
using zetta::cloud::AwsConfig;
using zetta::cloud::BareMetalCliControlPlane;
using zetta::cloud::BareMetalConfig;
using zetta::cloud::CommandResult;
using zetta::cloud::CommandRunner;
using zetta::cloud::K8sCliControlPlane;
using zetta::cloud::K8sConfig;
using zetta::cloud::NodeBootstrapScriptOptions;
using zetta::cloud::ProvisionRequest;

namespace {

constexpr const char* kRoot =
    "4f1c2e3d4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";

struct Recorder {
    std::vector<std::vector<std::string>> calls;
    CommandRunner runner() {
        return [this](const std::vector<std::string>& argv) {
            calls.push_back(argv);
            return CommandResult{};
        };
    }
    bool any_argument(const std::string& value) const {
        return std::ranges::any_of(calls, [&](const std::vector<std::string>& argv) {
            return std::ranges::any_of(argv, [&](const std::string& argument) {
                return argument.find(value) != std::string::npos;
            });
        });
    }
};

TEST(BurstIdentityRoot, KubernetesPodsGetTheRootAsAnEnvVar) {
    Recorder recorder;
    K8sCliControlPlane control_plane{recorder.runner()};
    K8sConfig config;
    config.image = "ghcr.io/oramasearch/zetta:latest";
    config.seed_peer = "10.0.0.1:7777";
    config.identity_root = kRoot;
    control_plane.bring_up(config, ProvisionRequest{1, {}});
    EXPECT_TRUE(recorder.any_argument(std::string("ZETTA_IDENTITY_ROOT=") + kRoot));
}

TEST(BurstIdentityRoot, BareMetalWorkersGetTheRootAsAnEnvVar) {
    Recorder recorder;
    BareMetalCliControlPlane control_plane{recorder.runner()};
    BareMetalConfig config;
    config.image = "ghcr.io/oramasearch/zetta:latest";
    config.seed_peer = "10.0.0.1:7777";
    config.identity_root = kRoot;
    control_plane.bring_up(config, ProvisionRequest{1, {}});
    EXPECT_TRUE(recorder.any_argument(std::string("ZETTA_IDENTITY_ROOT=") + kRoot));
}

TEST(BurstIdentityRoot, AwsNodesGetTheRootAsAPropagatingTag) {
    Recorder recorder;
    AwsCliControlPlane control_plane{recorder.runner()};
    AwsConfig config;
    config.region = "us-west-2";
    config.launch_template = "zetta-burst-lt";
    config.subnets = "subnet-aaa";
    config.seed_peer = "10.0.0.1:7777";
    config.identity_root = kRoot;
    control_plane.bring_up(config, ProvisionRequest{1, {}});
    EXPECT_TRUE(recorder.any_argument(std::string("Key=ZETTA_IDENTITY_ROOT,Value=") +
                                      kRoot + ",PropagateAtLaunch=true"));
}

TEST(BurstIdentityRoot, TheBootScriptWritesTheRootIntoTheEnvironmentFile) {
    NodeBootstrapScriptOptions options;
    options.seed_peer = "10.0.0.1:7777";
    options.identity_root = kRoot;
    std::string reason;
    const std::optional<std::string> script =
        zetta::cloud::render_node_bootstrap_script(options, &reason);
    ASSERT_TRUE(script.has_value()) << reason;
    EXPECT_NE(script->find(std::string("ZETTA_IDENTITY_ROOT=") + kRoot + "\n"),
              std::string::npos);
    EXPECT_NE(script->find("printf 'ZETTA_IDENTITY_ROOT=%s\\n'"), std::string::npos);
}

TEST(BurstIdentityRoot, NoRootAddsNothing) {
    Recorder recorder;
    K8sCliControlPlane control_plane{recorder.runner()};
    K8sConfig config;
    config.image = "ghcr.io/oramasearch/zetta:latest";
    config.seed_peer = "10.0.0.1:7777";
    control_plane.bring_up(config, ProvisionRequest{1, {}});
    EXPECT_FALSE(recorder.any_argument("ZETTA_IDENTITY_ROOT"));
}

}  // namespace
