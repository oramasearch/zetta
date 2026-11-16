// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../../src/core/base64.hpp"
#include "temp_dir.hpp"
#include "secret_argv.hpp"
#include "zetta/cloud/command_runner.hpp"
#include "zetta/cloud/node_bootstrap_script.hpp"

// #1316: the boot script a burst VM runs carries only the *name* of the secret
// holding the cluster join token, fetches the token with the VM's identity, and
// writes it to a 0400 file the engine reads through ZETTA_CLUSTER_TOKEN_FILE.
// The script is rendered and checked here, then run under /bin/sh against fake
// vendor CLIs (`curl`, `oci`, `aws`, `systemctl`) on PATH; no cloud is called.

using zetta::cloud::ClusterTokenReference;
using zetta::cloud::ClusterTokenStore;
using zetta::cloud::CommandResult;
using zetta::cloud::NodeBootstrapScriptOptions;
using zetta::cloud::cluster_token_secret_name;
using zetta::cloud::default_command_runner;
using zetta::cloud::render_node_bootstrap_script;
using zetta::cloud::shell_quote;

namespace fs = std::filesystem;

namespace {

constexpr const char* kToken =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char* kAccessToken = "fake-access-token-not-the-join-token";
constexpr const char* kIdentity =
    "/subscriptions/sub-123/resourceGroups/rg/providers/"
    "Microsoft.ManagedIdentity/userAssignedIdentities/zetta-burst";

std::string base64(std::string_view text) {
    return zetta::detail::base64_encode(std::as_bytes(std::span{text}));
}

void write_executable(const fs::path& path, const std::string& body) {
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "#!/bin/sh\n" << body;
    }
    fs::permissions(path, fs::perms::owner_all);
}

// A VM sandbox: a fake bin directory that shadows the vendor CLIs and the
// system tools the script must not really call, the token / env paths, and a
// log of every argv the fakes saw.
struct Sandbox {
    zetta::test::TempDir root;
    fs::path bin = root.path() / "bin";
    fs::path etc = root.path() / "etc" / "zetta";
    fs::path argv_log = root.path() / "argv.log";
    fs::path systemctl_log = root.path() / "systemctl.log";

    Sandbox() {
        fs::create_directories(bin);
        // Every fake appends its argv to the log, one call per line.
        write_executable(bin / "logger", "exit 0\n");
        write_executable(bin / "id", "exit 1\n");        // no zetta user ...
        write_executable(bin / "useradd", "exit 0\n");   // ... and none created
        write_executable(bin / "systemctl",
                         "echo \"$*\" >> '" + systemctl_log.string() + "'\nexit 0\n");
    }

    // A fake vendor CLI. `body` runs after the argv is logged.
    void fake(const std::string& name, const std::string& body) const {
        write_executable(bin / name,
                         "echo \"" + name + " $*\" >> '" + argv_log.string() + "'\n" + body);
    }

    NodeBootstrapScriptOptions options(ClusterTokenReference token) const {
        NodeBootstrapScriptOptions options;
        options.seed_peer = "10.0.0.1:7777";
        options.token = std::move(token);
        options.token_path = (etc / "cluster-token").string();
        options.env_path = (etc / "bootstrap.env").string();
        options.attempts = 3;
        options.retry_delay_seconds = 0;
        return options;
    }

    CommandResult run(const NodeBootstrapScriptOptions& options,
                      const std::string& shell = "/bin/sh") const {
        std::string reason;
        const std::optional<std::string> script = render_node_bootstrap_script(options, &reason);
        EXPECT_TRUE(script.has_value()) << reason;
        const fs::path path = root.path() / "bootstrap.sh";
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << script.value_or("exit 99\n");
        }
        return default_command_runner()({"/usr/bin/env",
                                         "PATH=" + bin.string() + ":/usr/bin:/bin",
                                         shell, path.string()});
    }

    std::string token_file() const {
        return zetta::test::read_whole_file(etc / "cluster-token");
    }
    std::string env_file() const {
        return zetta::test::read_whole_file(etc / "bootstrap.env");
    }
    std::string argv() const { return zetta::test::read_whole_file(argv_log); }
};

// A fake `curl` that answers the Azure IMDS token request (only for the expected
// managed identity) and the Key Vault secret GET (only with the bearer header
// file holding that access token).
std::string azure_curl(const std::string& vault, const std::string& secret) {
    return R"SH(
header=''
identity_ok=no
previous=''
for argument in "$@"; do
    if [ "$previous" = "-H" ]; then
        case $argument in @*) header=$(cat "${argument#@}") ;; esac
    fi
    case $argument in "msi_res_id=)SH" + std::string{kIdentity} + R"SH(") identity_ok=yes ;; esac
    previous=$argument
done
for argument in "$@"; do
    case $argument in
        http://169.254.169.254/metadata/identity/oauth2/token)
            [ "$identity_ok" = yes ] || exit 22
            echo '{"access_token":")SH" + std::string{kAccessToken} + R"SH(","expires_in":"3599"}'
            exit 0 ;;
        "https://)SH" + vault + ".vault.azure.net/secrets/" + secret + R"SH(?api-version=7.4")
            [ "$header" = "Authorization: Bearer )SH" + std::string{kAccessToken} + R"SH(" ] || exit 22
            echo '{"value":")SH" + std::string{kToken} + R"SH(","id":"https://vault/secrets/x/1"}'
            exit 0 ;;
    esac
done
exit 22
)SH";
}

ClusterTokenReference azure_reference() {
    ClusterTokenReference token;
    token.store = ClusterTokenStore::azure_key_vault;
    token.secret_name = "zetta-burst-cluster-token";
    token.container = "zetta-vault";
    token.identity = kIdentity;
    return token;
}

void expect_token_written(const Sandbox& sandbox) {
    EXPECT_EQ(sandbox.token_file(), std::string{kToken} + "\n");
    const fs::perms permissions = fs::status(sandbox.etc / "cluster-token").permissions();
    EXPECT_EQ(permissions & fs::perms::all, fs::perms::owner_read);
    const std::string env = sandbox.env_file();
    EXPECT_NE(env.find("ZETTA_SEED_PEER=10.0.0.1:7777\n"), std::string::npos) << env;
    EXPECT_NE(env.find("ZETTA_CLUSTER_TOKEN_FILE=" + (sandbox.etc / "cluster-token").string()),
              std::string::npos)
        << env;
    EXPECT_EQ(env.find(kToken), std::string::npos);
    EXPECT_EQ(env.find("ZETTA_CLUSTER_TOKEN="), std::string::npos);
    EXPECT_NE(zetta::test::read_whole_file(sandbox.systemctl_log).find("restart zetta.service"),
              std::string::npos);
}

}  // namespace

// ---- rendering --------------------------------------------------------------

TEST(NodeBootstrapScript, SecretNameIsThePoolNamePlusClusterToken) {
    EXPECT_EQ(cluster_token_secret_name("zetta-burst"), "zetta-burst-cluster-token");
}

TEST(NodeBootstrapScript, ShellQuoteLeavesPlainWordsAndQuotesTheRest) {
    EXPECT_EQ(shell_quote("10.0.0.1:7777"), "10.0.0.1:7777");
    EXPECT_EQ(shell_quote("/subscriptions/a-b/c"), "/subscriptions/a-b/c");
    EXPECT_EQ(shell_quote(""), "''");
    EXPECT_EQ(shell_quote("a b"), "'a b'");
    EXPECT_EQ(shell_quote("$(reboot)"), "'$(reboot)'");
    EXPECT_EQ(shell_quote("it's"), "'it'\\''s'");
}

TEST(NodeBootstrapScript, CarriesTheSecretNameAndNeverAToken) {
    for (const ClusterTokenStore store :
         {ClusterTokenStore::azure_key_vault, ClusterTokenStore::gcp_secret_manager,
          ClusterTokenStore::oci_vault, ClusterTokenStore::aws_ssm_parameter}) {
        ClusterTokenReference token = azure_reference();
        token.store = store;
        NodeBootstrapScriptOptions options;
        options.seed_peer = "10.0.0.1:7777";
        options.token = token;
        std::string reason;
        const std::optional<std::string> script = render_node_bootstrap_script(options, &reason);
        ASSERT_TRUE(script.has_value()) << reason;
        EXPECT_TRUE(script->starts_with("#!/bin/sh\n"));
        EXPECT_NE(script->find("TOKEN_SECRET=zetta-burst-cluster-token\n"), std::string::npos);
        EXPECT_NE(script->find("ZETTA_SEED_PEER=10.0.0.1:7777\n"), std::string::npos);
        EXPECT_NE(script->find("TOKEN_FILE=/etc/zetta/cluster-token\n"), std::string::npos);
        EXPECT_NE(script->find("ENV_FILE=/etc/zetta/bootstrap.env\n"), std::string::npos);
        EXPECT_EQ(script->find("ZETTA_CLUSTER_TOKEN="), std::string::npos);
        EXPECT_NE(script->find("fetch_token() {"), std::string::npos);
    }
}

TEST(NodeBootstrapScript, AValueCannotBreakOutOfItsQuotes) {
    NodeBootstrapScriptOptions options;
    options.seed_peer = "10.0.0.1:7777'; reboot; echo '";
    std::string reason;
    const std::optional<std::string> script = render_node_bootstrap_script(options, &reason);
    ASSERT_TRUE(script.has_value()) << reason;
    EXPECT_NE(script->find("ZETTA_SEED_PEER='10.0.0.1:7777'\\''; reboot; echo '\\'''\n"),
              std::string::npos)
        << *script;
}

TEST(NodeBootstrapScript, RefusesControlCharactersAndMissingFields) {
    std::string reason;
    NodeBootstrapScriptOptions options;
    options.seed_peer = "10.0.0.1:7777\nexport X=1";
    EXPECT_FALSE(render_node_bootstrap_script(options, &reason).has_value());
    EXPECT_NE(reason.find("control character"), std::string::npos);

    options = {};
    options.token = azure_reference();
    options.token.identity.clear();
    EXPECT_FALSE(render_node_bootstrap_script(options, &reason).has_value());
    EXPECT_NE(reason.find("managed identity"), std::string::npos);

    options.token = azure_reference();
    options.token.container.clear();
    EXPECT_FALSE(render_node_bootstrap_script(options, &reason).has_value());

    options.token = azure_reference();
    options.token.store = ClusterTokenStore::gcp_secret_manager;
    options.token.container.clear();
    EXPECT_FALSE(render_node_bootstrap_script(options, &reason).has_value());

    options.token.store = ClusterTokenStore::oci_vault;
    EXPECT_FALSE(render_node_bootstrap_script(options, &reason).has_value());

    options.token = azure_reference();
    options.token.secret_name.clear();
    EXPECT_FALSE(render_node_bootstrap_script(options, &reason).has_value());
}

// ---- running the script against fake vendor CLIs -----------------------------

TEST(NodeBootstrapScript, AzureFetchesFromKeyVaultWithTheManagedIdentity) {
    const Sandbox sandbox;
    sandbox.fake("curl", azure_curl("zetta-vault", "zetta-burst-cluster-token"));

    const CommandResult ran = sandbox.run(sandbox.options(azure_reference()));
    ASSERT_EQ(ran.exit_code, 0) << ran.stdout_text << ran.stderr_text;
    expect_token_written(sandbox);
    // Neither the IMDS access token nor the join token was ever an argument.
    EXPECT_EQ(sandbox.argv().find(kAccessToken), std::string::npos) << sandbox.argv();
    EXPECT_EQ(sandbox.argv().find(kToken), std::string::npos);
}

// Debian and Ubuntu run user-data with dash, not bash: the script is POSIX sh.
TEST(NodeBootstrapScript, RunsUnderDash) {
    if (!fs::exists("/bin/dash")) GTEST_SKIP() << "no /bin/dash on this host";
    const Sandbox sandbox;
    sandbox.fake("curl", azure_curl("zetta-vault", "zetta-burst-cluster-token"));
    const CommandResult ran = sandbox.run(sandbox.options(azure_reference()), "/bin/dash");
    ASSERT_EQ(ran.exit_code, 0) << ran.stdout_text << ran.stderr_text;
    expect_token_written(sandbox);
}

TEST(NodeBootstrapScript, GcpFetchesFromSecretManagerWithTheServiceAccount) {
    const Sandbox sandbox;
    sandbox.fake("curl", R"SH(
header=''
previous=''
for argument in "$@"; do
    if [ "$previous" = "-H" ]; then
        case $argument in @*) header=$(cat "${argument#@}") ;; esac
    fi
    previous=$argument
done
for argument in "$@"; do
    case $argument in
        http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token)
            echo '{"access_token":")SH" + std::string{kAccessToken} + R"SH(","expires_in":3599,"token_type":"Bearer"}'
            exit 0 ;;
        https://secretmanager.googleapis.com/v1/projects/zetta-project/secrets/zetta-burst-cluster-token/versions/latest:access)
            [ "$header" = "Authorization: Bearer )SH" + std::string{kAccessToken} + R"SH(" ] || exit 22
            printf '{\n  "name": "projects/1/secrets/zetta-burst-cluster-token/versions/3",\n  "payload": {\n    "data": ")SH" + base64(kToken) + R"SH(",\n    "dataCrc32c": "1"\n  }\n}\n'
            exit 0 ;;
    esac
done
exit 22
)SH");

    ClusterTokenReference token;
    token.store = ClusterTokenStore::gcp_secret_manager;
    token.secret_name = "zetta-burst-cluster-token";
    token.container = "zetta-project";
    const CommandResult ran = sandbox.run(sandbox.options(token));
    ASSERT_EQ(ran.exit_code, 0) << ran.stdout_text << ran.stderr_text;
    expect_token_written(sandbox);
    EXPECT_EQ(sandbox.argv().find(kAccessToken), std::string::npos) << sandbox.argv();
}

TEST(NodeBootstrapScript, OciFetchesFromVaultWithTheInstancePrincipal) {
    const Sandbox sandbox;
    sandbox.fake("oci", R"SH(
case " $* " in
    *" secrets secret-bundle get-secret-bundle-by-name "*) ;;
    *) exit 2 ;;
esac
case " $* " in *" --auth instance_principal "*) ;; *) exit 2 ;; esac
case " $* " in *" --vault-id ocid1.vault.oc1..v "*) ;; *) exit 2 ;; esac
case " $* " in *" --secret-name zetta-burst-cluster-token "*) ;; *) exit 2 ;; esac
case " $* " in *" --region us-ashburn-1 "*) ;; *) exit 2 ;; esac
echo ')SH" + base64(kToken) + R"SH('
)SH");

    ClusterTokenReference token;
    token.store = ClusterTokenStore::oci_vault;
    token.secret_name = "zetta-burst-cluster-token";
    token.container = "ocid1.vault.oc1..v";
    token.region = "us-ashburn-1";
    const CommandResult ran = sandbox.run(sandbox.options(token));
    ASSERT_EQ(ran.exit_code, 0) << ran.stdout_text << ran.stderr_text;
    expect_token_written(sandbox);
}

TEST(NodeBootstrapScript, AwsFetchesTheSsmSecureString) {
    const Sandbox sandbox;
    sandbox.fake("aws", R"SH(
case " $* " in
    *" ssm get-parameter --name /zetta/zetta-burst/cluster-token --with-decryption "*) ;;
    *) exit 255 ;;
esac
case " $* " in *" --region us-west-2 "*) ;; *) exit 255 ;; esac
echo ')SH" + std::string{kToken} + R"SH('
)SH");

    ClusterTokenReference token;
    token.store = ClusterTokenStore::aws_ssm_parameter;
    token.secret_name = "/zetta/zetta-burst/cluster-token";
    token.region = "us-west-2";
    const CommandResult ran = sandbox.run(sandbox.options(token));
    ASSERT_EQ(ran.exit_code, 0) << ran.stdout_text << ran.stderr_text;
    expect_token_written(sandbox);
}

// An IAM grant on a fresh identity can lag the VM's boot: the script retries.
TEST(NodeBootstrapScript, RetriesUntilTheGrantPropagates) {
    const Sandbox sandbox;
    const fs::path counter = sandbox.root.path() / "count";
    sandbox.fake("aws", "count=$(cat '" + counter.string() + "' 2>/dev/null || echo 0)\n"
                        "count=$((count + 1))\n"
                        "echo \"$count\" > '" + counter.string() + "'\n"
                        "[ \"$count\" -ge 3 ] || exit 255\n"
                        "echo '" + std::string{kToken} + "'\n");
    ClusterTokenReference token;
    token.store = ClusterTokenStore::aws_ssm_parameter;
    token.secret_name = "/zetta/zetta-burst/cluster-token";
    const CommandResult ran = sandbox.run(sandbox.options(token));
    ASSERT_EQ(ran.exit_code, 0) << ran.stdout_text << ran.stderr_text;
    EXPECT_EQ(sandbox.token_file(), std::string{kToken} + "\n");
    // The default runner merges stderr into stdout.
    const std::string output = ran.stdout_text + ran.stderr_text;
    EXPECT_NE(output.find("attempt 2"), std::string::npos) << output;
}

// A store that never answers with a valid token stops the boot: no token file,
// no environment file, no engine start.
TEST(NodeBootstrapScript, AMalformedTokenIsNeverWritten) {
    const Sandbox sandbox;
    sandbox.fake("aws", "echo 'not-a-join-token'\n");
    ClusterTokenReference token;
    token.store = ClusterTokenStore::aws_ssm_parameter;
    token.secret_name = "/zetta/zetta-burst/cluster-token";
    const CommandResult ran = sandbox.run(sandbox.options(token));
    EXPECT_NE(ran.exit_code, 0);
    EXPECT_FALSE(fs::exists(sandbox.etc / "cluster-token"));
    EXPECT_FALSE(fs::exists(sandbox.etc / "bootstrap.env"));
    EXPECT_FALSE(fs::exists(sandbox.systemctl_log));
    const std::string output = ran.stdout_text + ran.stderr_text;
    EXPECT_NE(output.find("no valid cluster join token"), std::string::npos) << output;
}

// An unsecured (test / embedded) cluster: only the seed is handed over.
TEST(NodeBootstrapScript, WithoutAStoreOnlyTheSeedIsWritten) {
    const Sandbox sandbox;
    const CommandResult ran = sandbox.run(sandbox.options(ClusterTokenReference{}));
    ASSERT_EQ(ran.exit_code, 0) << ran.stdout_text << ran.stderr_text;
    EXPECT_EQ(sandbox.env_file(), "ZETTA_SEED_PEER=10.0.0.1:7777\n");
    EXPECT_FALSE(fs::exists(sandbox.etc / "cluster-token"));
}
