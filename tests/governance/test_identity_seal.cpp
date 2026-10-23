// The identity root (#763, #764): sealing identity records, opening them, and
// pinning the root in the data dir. The cluster-level proofs (a joining node
// mints no owner, a member's raw writes are not honored) are in
// tests/cluster/test_cluster_join_authority.cpp; the regression probes are in
// test_authz_regressions.cpp. See docs/governance/identity-seal.md.
#include <gtest/gtest.h>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/identity_seal.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::IdentityAuthority;
using zetta::governance::kIdentitySealField;
using zetta::governance::load_or_create_identity_root;
using zetta::test::TempDir;

namespace {

IdentityAuthority root_from(const std::string& secret) {
    auto root = IdentityAuthority::from_owner_secret(secret);
    EXPECT_TRUE(root);
    return *root;
}

const nlohmann::json kUser{{"name", "alice"}, {"status", "active"},
                           {"created", "2026-10-21T00:00:00Z"}};

std::filesystem::path root_file(const TempDir& dir) {
    return dir.path() / "security" / "identity-root.json";
}

}  // namespace

TEST(IdentitySeal, TheSameOwnerTokenDerivesTheSameRoot) {
    const IdentityAuthority a = root_from("owner-token");
    const IdentityAuthority b = root_from("owner-token");
    const IdentityAuthority other = root_from("another-token");
    EXPECT_EQ(a.public_key_hex(), b.public_key_hex());
    EXPECT_NE(a.public_key_hex(), other.public_key_hex());
    EXPECT_EQ(a.public_key_hex().size(), 64u);
    EXPECT_TRUE(a.can_seal());
    EXPECT_FALSE(IdentityAuthority::from_owner_secret(""));
}

TEST(IdentitySeal, ASealedRecordOpensAndLosesItsSeal) {
    const IdentityAuthority root = root_from("owner-token");
    const auto sealed = root.seal("user:1", kUser);
    ASSERT_TRUE(sealed);
    ASSERT_TRUE(sealed->contains(std::string(kIdentitySealField)));
    const auto opened = root.open("user:1", *sealed);
    ASSERT_TRUE(opened);
    EXPECT_EQ(*opened, kUser);
}

TEST(IdentitySeal, AnyChangeBreaksTheSeal) {
    const IdentityAuthority root = root_from("owner-token");
    const nlohmann::json sealed = *root.seal("user:1", kUser);

    // An unsealed record, a changed field, an added field, another key, a seal
    // from another root, and a garbled seal all fail to open.
    EXPECT_FALSE(root.open("user:1", kUser));
    nlohmann::json renamed = sealed;
    renamed["name"] = "mallory";
    EXPECT_FALSE(root.open("user:1", renamed));
    nlohmann::json widened = sealed;
    widened["extra"] = true;
    EXPECT_FALSE(root.open("user:1", widened));
    EXPECT_FALSE(root.open("user:2", sealed));
    EXPECT_FALSE(root.open("role:1", sealed));
    const nlohmann::json foreign = *root_from("another-token").seal("user:1", kUser);
    EXPECT_FALSE(root.open("user:1", foreign));
    nlohmann::json garbled = sealed;
    garbled[std::string(kIdentitySealField)] = "ed25519-v1:00";
    EXPECT_FALSE(root.open("user:1", garbled));
    EXPECT_FALSE(root.open("user:1", nlohmann::json::array()));
}

TEST(IdentitySeal, AVerifyOnlyRootOpensButCannotSeal) {
    const IdentityAuthority root = root_from("owner-token");
    const auto verifier = IdentityAuthority::verify_only(root.public_key_hex());
    ASSERT_TRUE(verifier);
    EXPECT_FALSE(verifier->can_seal());
    EXPECT_FALSE(verifier->seal("user:1", kUser));
    EXPECT_TRUE(verifier->open("user:1", *root.seal("user:1", kUser)));
    EXPECT_FALSE(IdentityAuthority::verify_only("not-hex"));
    EXPECT_FALSE(IdentityAuthority::verify_only(std::string(64, 'A')));
}

TEST(IdentitySeal, DerivedIdsAreFixedByTheRootAndUuidShaped) {
    const IdentityAuthority a = root_from("owner-token");
    const IdentityAuthority b = root_from("owner-token");
    const IdentityAuthority other = root_from("another-token");
    const std::string id = a.derived_id("owner-user");
    EXPECT_EQ(id, b.derived_id("owner-user"));
    EXPECT_NE(id, other.derived_id("owner-user"));
    EXPECT_NE(id, a.derived_id("owner-token"));
    ASSERT_EQ(id.size(), 36u);
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[14], '8');  // RFC 9562 version 8
}

TEST(IdentitySeal, IdentityKeysAreTheSevenIdentityPrefixes) {
    for (const char* key : {"user:1", "role:1", "token:1", "user_role:1",
                            "policy:1", "principal:1", "key:1"}) {
        EXPECT_TRUE(zetta::governance::is_identity_key(key)) << key;
    }
    for (const char* key : {"db:1", "tbl:1", "row:1", "node:1", "project:1"}) {
        EXPECT_FALSE(zetta::governance::is_identity_key(key)) << key;
    }
}

// ----- pinning the root (<data_dir>/security/identity-root.json) -------------

TEST(IdentityRoot, TheFirstStartPinsTheOwnerTokensRoot) {
    TempDir dir;
    std::string reason;
    const auto setup = load_or_create_identity_root(dir.path().string(),
                                                    "owner-token", "", &reason);
    ASSERT_TRUE(setup.authority) << reason;
    EXPECT_TRUE(setup.pinned_now);
    EXPECT_TRUE(setup.authority->can_seal());
    EXPECT_EQ(setup.authority->public_key_hex(),
              root_from("owner-token").public_key_hex());
    // Only the public key is written: the owner token is the private half.
    std::ifstream in(root_file(dir));
    const nlohmann::json file = nlohmann::json::parse(in);
    EXPECT_FALSE(file.contains("seed"));

    // A restart with the same token issues identity again.
    const auto again = load_or_create_identity_root(dir.path().string(),
                                                    "owner-token", "", &reason);
    ASSERT_TRUE(again.authority);
    EXPECT_FALSE(again.pinned_now);
    EXPECT_TRUE(again.authority->can_seal());
}

TEST(IdentityRoot, ADifferentTokenLaterIsVerifyOnlyWithAWarning) {
    TempDir dir;
    ASSERT_TRUE(load_or_create_identity_root(dir.path().string(), "owner-token", "")
                    .authority);
    std::string reason;
    const auto setup = load_or_create_identity_root(dir.path().string(),
                                                    "another-token", "", &reason);
    ASSERT_TRUE(setup.authority) << reason;
    EXPECT_FALSE(setup.authority->can_seal());
    EXPECT_EQ(setup.authority->public_key_hex(),
              root_from("owner-token").public_key_hex());
    EXPECT_FALSE(setup.warning.empty());
}

TEST(IdentityRoot, NoOwnerTokenGeneratesAPrivateRootThatSurvivesARestart) {
    TempDir dir;
    std::string reason;
    const auto setup =
        load_or_create_identity_root(dir.path().string(), "", "", &reason);
    ASSERT_TRUE(setup.authority) << reason;
    EXPECT_TRUE(setup.authority->can_seal());
    struct stat info {};
    ASSERT_EQ(::stat(root_file(dir).c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);  // it holds the seed
    const auto again =
        load_or_create_identity_root(dir.path().string(), "", "", &reason);
    ASSERT_TRUE(again.authority);
    EXPECT_TRUE(again.authority->can_seal());
    EXPECT_EQ(again.authority->public_key_hex(), setup.authority->public_key_hex());
}

TEST(IdentityRoot, AnIdentityRootFlagPinsAVerifyOnlyRoot) {
    TempDir dir;
    const std::string pub = root_from("owner-token").public_key_hex();
    std::string reason;
    const auto setup =
        load_or_create_identity_root(dir.path().string(), "", pub, &reason);
    ASSERT_TRUE(setup.authority) << reason;
    EXPECT_FALSE(setup.authority->can_seal());
    EXPECT_EQ(setup.authority->public_key_hex(), pub);

    // With the matching owner token as well, it issues identity.
    TempDir both;
    const auto issuer = load_or_create_identity_root(both.path().string(),
                                                     "owner-token", pub, &reason);
    ASSERT_TRUE(issuer.authority) << reason;
    EXPECT_TRUE(issuer.authority->can_seal());
}

TEST(IdentityRoot, AFlagThatDiffersFromThePinIsRefused) {
    TempDir dir;
    ASSERT_TRUE(load_or_create_identity_root(dir.path().string(), "owner-token", "")
                    .authority);
    std::string reason;
    const auto setup = load_or_create_identity_root(
        dir.path().string(), "", root_from("another-token").public_key_hex(), &reason);
    EXPECT_FALSE(setup.authority);
    EXPECT_NE(reason.find("differs"), std::string::npos) << reason;
    EXPECT_FALSE(
        load_or_create_identity_root(dir.path().string(), "", "zz", &reason).authority);
}

TEST(IdentityRoot, AMalformedPinIsRefusedNotOverwritten) {
    TempDir dir;
    std::filesystem::create_directories(root_file(dir).parent_path());
    std::ofstream(root_file(dir)) << R"({"version":1,"public_key":"nope"})";
    std::string reason;
    EXPECT_FALSE(load_or_create_identity_root(dir.path().string(), "owner-token", "",
                                              &reason)
                     .authority);
    EXPECT_NE(reason.find("identity-root.json"), std::string::npos) << reason;
    std::ifstream in(root_file(dir));
    std::string contents((std::istreambuf_iterator<char>(in)), {});
    EXPECT_NE(contents.find("nope"), std::string::npos);
}

// ----- the node: writes are sealed, unsealed records are not honored ----------

namespace {

NodeConfig config_with(const TempDir& dir,
                       std::optional<IdentityAuthority> root) {
    NodeConfig config;
    config.data_dir = dir.path().string();
    config.preflight = false;
    config.identity_authority = std::move(root);
    return config;
}

}  // namespace

TEST(IdentitySealNode, EveryIdentityWriteIsSealed) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, root_from("owner-token")), &replication);
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner("owner-token").performed);
    auto user = node.create_user("alice");
    ASSERT_TRUE(user);
    auto role = node.create_role("analyst");
    ASSERT_TRUE(role);
    ASSERT_TRUE(node.grant_permissions("analyst", {"sql:read"}));
    ASSERT_TRUE(node.assign_role(user->id(), role->id()));
    ASSERT_TRUE(node.create_token(user->id(), zetta::governance::TokenType::Access).token);
    zetta::governance::PolicyValue policy;
    policy.name = "own-rows";
    policy.database = "db";
    policy.table = "t";
    policy.command = "select";
    ASSERT_TRUE(node.create_policy(policy));
    auto principal = node.create_principal("svc", zetta::governance::PrincipalKind::Service);
    ASSERT_TRUE(principal);
    ASSERT_TRUE(principal->create_key(zetta::governance::Scope{}));

    std::size_t identity = 0;
    for (const auto& [key, value] : replication.snapshot().items()) {
        if (!zetta::governance::is_identity_key(key)) continue;
        ++identity;
        EXPECT_TRUE(node.identity_authority()->open(key, value)) << key;
    }
    EXPECT_GE(identity, 10u);
    node.stop();
}

TEST(IdentitySealNode, TheOwnerAndBuiltinRolesHaveIdsFixedByTheRoot) {
    const IdentityAuthority root = root_from("owner-token");
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a(zetta::util::new_uuid());
    ReplicationManager repl_b(zetta::util::new_uuid());
    Node a(config_with(dir_a, root), &repl_a);
    Node b(config_with(dir_b, root), &repl_b);
    ASSERT_TRUE(a.start());
    ASSERT_TRUE(b.start());
    const auto boot_a = a.bootstrap_owner("owner-token");
    const auto boot_b = b.bootstrap_owner("owner-token");
    ASSERT_TRUE(boot_a.performed && boot_b.performed);
    EXPECT_EQ(boot_a.user_id, boot_b.user_id);
    EXPECT_EQ(boot_a.token_id, boot_b.token_id);
    EXPECT_EQ(a.find_role("admin")->id(), b.find_role("admin")->id());
    // Two nodes that seeded before they ever met merge into one owner.
    repl_a.merge(repl_b.save());
    a.tick();
    std::size_t owners = 0;
    for (const auto& user : a.users()) owners += user->name() == "owner";
    EXPECT_EQ(owners, 1u);
    std::size_t admins = 0;
    for (const auto& role : a.roles()) admins += role->name() == "admin";
    EXPECT_EQ(admins, 1u);
    EXPECT_TRUE(a.authenticate_token("owner-token"));
    a.stop();
    b.stop();
}

TEST(IdentitySealNode, AVerifyOnlyNodeIssuesNoIdentity) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, IdentityAuthority::verify_only(
                                   root_from("owner-token").public_key_hex())),
              &replication);
    ASSERT_TRUE(node.start());
    EXPECT_FALSE(node.can_issue_identity());
    EXPECT_FALSE(node.bootstrap_owner("owner-token").performed);
    EXPECT_FALSE(node.create_user("alice"));
    EXPECT_FALSE(node.create_role("analyst"));
    EXPECT_EQ(node.ensure_builtin_roles(), 0u);
    EXPECT_FALSE(node.create_principal("svc", zetta::governance::PrincipalKind::Service));
    for (const auto& [key, value] : replication.snapshot().items()) {
        EXPECT_FALSE(zetta::governance::is_identity_key(key)) << key;
    }
    // SQL says why, instead of "already exists".
    zetta::query::QueryService svc{node};
    const auto refused = svc.run("CREATE USER alice;", "");
    EXPECT_FALSE(refused.ok());
    EXPECT_NE(refused.error.find("identity root"), std::string::npos) << refused.error;
    node.stop();
}

TEST(IdentitySealNode, AnUnsealedRecordInTheDocumentIsNotMaterialized) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, root_from("owner-token")), &replication);
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner("owner-token").performed);
    const std::size_t users = node.user_count();
    replication.set("user:" + zetta::util::new_uuid(), kUser);
    node.tick();
    EXPECT_EQ(node.user_count(), users);
    EXPECT_FALSE(node.find_user("alice"));

    // Tampering with a sealed record removes it rather than changing it.
    auto bob = node.create_user("bob");
    ASSERT_TRUE(bob);
    nlohmann::json tampered = replication.snapshot()["user:" + bob->id()];
    tampered["name"] = "bob-the-admin";
    replication.set("user:" + bob->id(), tampered);
    node.tick();
    EXPECT_FALSE(node.find_user("bob-the-admin"));
    EXPECT_FALSE(node.find_user_by_id(bob->id()));
    node.stop();
}

TEST(IdentitySealNode, RecordsHeldBeforeTheRootAreSealedOnceOnTheFirstStart) {
    TempDir dir;
    std::string legacy_user_id;
    {
        // A node from before the root: identity replicated unsealed.
        ReplicationManager replication("11111111-1111-4111-8111-111111111111");
        Node node(config_with(dir, std::nullopt), &replication);
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner("owner-token").performed);
        legacy_user_id = node.create_user("alice")->id();
        node.stop();
    }
    {
        // The upgrade: the same data dir under a root keeps its accounts.
        ReplicationManager replication("11111111-1111-4111-8111-111111111111");
        Node node(config_with(dir, root_from("owner-token")), &replication);
        ASSERT_TRUE(node.start());
        EXPECT_TRUE(node.find_user_by_id(legacy_user_id));
        EXPECT_TRUE(node.authenticate_token("owner-token"));
        EXPECT_TRUE(std::filesystem::exists(dir.path() / "security" / "identity-sealed"));

        // Once done, a record that arrives unsealed later is not blessed.
        replication.set("user:" + zetta::util::new_uuid(),
                        nlohmann::json{{"name", "late"}, {"status", "active"},
                                       {"created", "2026-10-21T00:00:00Z"}});
        node.tick();
        EXPECT_FALSE(node.find_user("late"));
        node.stop();
    }
    {
        ReplicationManager replication("11111111-1111-4111-8111-111111111111");
        Node node(config_with(dir, root_from("owner-token")), &replication);
        ASSERT_TRUE(node.start());
        EXPECT_FALSE(node.find_user("late"));
        EXPECT_TRUE(node.find_user_by_id(legacy_user_id));
        node.stop();
    }
}
