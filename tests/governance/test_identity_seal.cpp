// The identity root (#763, #764): sealing identity records, opening them, and
// pinning the root in the data dir. Versions, sealed removals and refusing an
// older record (#1341) are here too. The cluster-level proofs (a joining node
// mints no owner, a member's raw writes are not honored) are in
// tests/cluster/test_cluster_join_authority.cpp; the regression probes are in
// test_authz_regressions.cpp. See docs/governance/identity-seal.md.
#include <gtest/gtest.h>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

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
using zetta::governance::kIdentityTombstoneField;
using zetta::governance::kIdentityVersionField;
using zetta::governance::OpenedIdentityRecord;
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

TEST(IdentitySeal, ASealedRecordOpensAndLosesItsSealAndVersion) {
    const IdentityAuthority root = root_from("owner-token");
    const auto sealed = root.seal("user:1", kUser, 7);
    ASSERT_TRUE(sealed);
    ASSERT_TRUE(sealed->contains(std::string(kIdentitySealField)));
    EXPECT_EQ((*sealed)[std::string(kIdentityVersionField)], 7);
    const auto opened = root.open("user:1", *sealed);
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->value, kUser);
    EXPECT_EQ(opened->version, 7u);
    EXPECT_FALSE(opened->removed);
    EXPECT_FALSE(root.seal("user:1", kUser, 0)) << "version zero is refused";
}

TEST(IdentitySeal, AnyChangeBreaksTheSeal) {
    const IdentityAuthority root = root_from("owner-token");
    const nlohmann::json sealed = *root.seal("user:1", kUser, 1);

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
    const nlohmann::json foreign = *root_from("another-token").seal("user:1", kUser, 1);
    EXPECT_FALSE(root.open("user:1", foreign));
    nlohmann::json garbled = sealed;
    garbled[std::string(kIdentitySealField)] = "ed25519-v1:00";
    EXPECT_FALSE(root.open("user:1", garbled));
    EXPECT_FALSE(root.open("user:1", nlohmann::json::array()));
    // The version is sealed too: changed, removed or not a positive integer.
    nlohmann::json bumped = sealed;
    bumped[std::string(kIdentityVersionField)] = 2;
    EXPECT_FALSE(root.open("user:1", bumped));
    nlohmann::json unversioned = sealed;
    unversioned.erase(std::string(kIdentityVersionField));
    EXPECT_FALSE(root.open("user:1", unversioned));
    nlohmann::json negative = sealed;
    negative[std::string(kIdentityVersionField)] = -1;
    EXPECT_FALSE(root.open("user:1", negative));
}

// #1341: a removal is a sealed record of its own. A tombstone-shaped value
// without the root's seal opens as nothing, and a live record cannot be sealed
// with the removal marker in it.
TEST(IdentitySeal, ARemovalIsASealedTombstone) {
    const IdentityAuthority root = root_from("owner-token");
    const auto tombstone = root.seal_removal("token:1", 9);
    ASSERT_TRUE(tombstone);
    const auto opened = root.open("token:1", *tombstone);
    ASSERT_TRUE(opened);
    EXPECT_TRUE(opened->removed);
    EXPECT_EQ(opened->version, 9u);
    EXPECT_FALSE(root.open("token:2", *tombstone)) << "a removal is bound to its key";

    const nlohmann::json forged{{std::string(kIdentityTombstoneField), true},
                                {std::string(kIdentityVersionField), 1000}};
    EXPECT_FALSE(root.open("token:1", forged));
    EXPECT_FALSE(root.open("token:1", nlohmann::json{{"_removed", true}}));
    nlohmann::json smuggled = kUser;
    smuggled[std::string(kIdentityTombstoneField)] = true;
    EXPECT_FALSE(root.seal("user:1", smuggled, 1));
    EXPECT_FALSE(IdentityAuthority::verify_only(root.public_key_hex())
                     ->seal_removal("token:1", 9));
}

TEST(IdentitySeal, ANewerRecordSupersedesAnOlderOneOnEveryNode) {
    const IdentityAuthority root = root_from("owner-token");
    const OpenedIdentityRecord v1 = *root.open("user:1", *root.seal("user:1", kUser, 1));
    const OpenedIdentityRecord v2 = *root.open("user:1", *root.seal("user:1", kUser, 2));
    const OpenedIdentityRecord gone = *root.open("user:1", *root.seal_removal("user:1", 3));
    EXPECT_TRUE(zetta::governance::identity_record_supersedes(v2, v1));
    EXPECT_FALSE(zetta::governance::identity_record_supersedes(v1, v2));
    EXPECT_TRUE(zetta::governance::identity_record_supersedes(gone, v2));
    EXPECT_FALSE(zetta::governance::identity_record_supersedes(v2, v2));
    // Two records of one version (two issuers at once) still order one way.
    nlohmann::json renamed = kUser;
    renamed["name"] = "alice2";
    const OpenedIdentityRecord other = *root.open("user:1", *root.seal("user:1", renamed, 2));
    EXPECT_NE(zetta::governance::identity_record_supersedes(v2, other),
              zetta::governance::identity_record_supersedes(other, v2));

    // The next version is newer than the document's record and the floor.
    const nlohmann::json far_future = *root.seal("user:1", kUser, 1ull << 62);
    EXPECT_EQ(zetta::governance::next_identity_version(root, "user:1", &far_future),
              (1ull << 62) + 1);
    EXPECT_EQ(zetta::governance::next_identity_version(root, "user:1", nullptr,
                                                       (1ull << 62) + 5),
              (1ull << 62) + 6);
    EXPECT_GT(zetta::governance::next_identity_version(root, "user:1", nullptr), 1u);
}

TEST(IdentitySeal, AVerifyOnlyRootOpensButCannotSeal) {
    const IdentityAuthority root = root_from("owner-token");
    const auto verifier = IdentityAuthority::verify_only(root.public_key_hex());
    ASSERT_TRUE(verifier);
    EXPECT_FALSE(verifier->can_seal());
    EXPECT_FALSE(verifier->seal("user:1", kUser, 1));
    EXPECT_TRUE(verifier->open("user:1", *root.seal("user:1", kUser, 1)));
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

    // Tampering with a sealed record neither changes it nor removes it: the
    // node keeps the sealed record it holds and puts it back (#1341).
    auto bob = node.create_user("bob");
    ASSERT_TRUE(bob);
    const nlohmann::json original = replication.snapshot()["user:" + bob->id()];
    nlohmann::json tampered = original;
    tampered["name"] = "bob-the-admin";
    replication.set("user:" + bob->id(), tampered);
    node.tick();
    EXPECT_FALSE(node.find_user("bob-the-admin"));
    ASSERT_TRUE(node.find_user_by_id(bob->id()));
    EXPECT_EQ(node.find_user_by_id(bob->id())->name(), "bob");
    EXPECT_EQ(replication.snapshot()["user:" + bob->id()], original);
    node.stop();
}

// #1341 on one node: a sealed record older than the one the node holds is
// refused, and a removal the node makes is a sealed tombstone.
TEST(IdentitySealNode, AnOlderSealedRecordIsRefused) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, root_from("owner-token")), &replication);
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner("owner-token").performed);
    auto alice = node.create_user("alice");
    ASSERT_TRUE(alice);
    const auto issued = node.create_token(alice->id(), zetta::governance::TokenType::Access);
    ASSERT_TRUE(issued.token);
    const std::string key = "token:" + issued.token->id();
    const nlohmann::json before = replication.snapshot()[key];

    ASSERT_TRUE(node.revoke_token(issued.token->id()));
    node.tick();
    replication.set(key, before);  // the replay
    node.tick();
    EXPECT_EQ(node.authenticate_token(issued.secret), nullptr);
    EXPECT_EQ(replication.snapshot()[key].value("revoked", false), true)
        << "the newer sealed record is put back";

    // A drop writes a sealed tombstone, not the CRDT's unsealed one.
    ASSERT_TRUE(node.drop_user("alice"));
    const nlohmann::json removed = replication.snapshot()["user:" + alice->id()];
    const auto opened = node.identity_authority()->open("user:" + alice->id(), removed);
    ASSERT_TRUE(opened);
    EXPECT_TRUE(opened->removed);
    node.tick();
    EXPECT_FALSE(node.find_user("alice"));
    node.stop();
}

// #1341 across a restart: the newest records a node accepted are kept in
// <data_dir>/security/identity-held.json, so an older sealed record written
// into its document while it was down is still refused.
TEST(IdentitySealNode, TheRecordsANodeHeldSurviveARestart) {
    TempDir dir;
    const std::string node_id = "22222222-2222-4222-8222-222222222222";
    std::string token_key;
    std::string secret;
    nlohmann::json before;
    {
        ReplicationManager replication(node_id);
        Node node(config_with(dir, root_from("owner-token")), &replication);
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner("owner-token").performed);
        auto alice = node.create_user("alice");
        ASSERT_TRUE(alice);
        const auto issued =
            node.create_token(alice->id(), zetta::governance::TokenType::Access);
        token_key = "token:" + issued.token->id();
        secret = issued.secret;
        before = replication.snapshot()[token_key];
        ASSERT_TRUE(node.revoke_token(issued.token->id()));
        node.tick();
        node.stop();
    }
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "security" / "identity-held.json"));
    {
        // While the node is down its persisted document gets the old record.
        const auto path = dir.path() / "replication.automerge";
        std::ifstream in(path, std::ios::binary);
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) bytes[i] = std::byte(raw[i]);
        ReplicationManager offline(node_id);
        ASSERT_TRUE(offline.restore(bytes));
        offline.set(token_key, before);
        const std::vector<std::byte> saved = offline.save();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(saved.data()),
                  static_cast<std::streamsize>(saved.size()));
    }
    {
        ReplicationManager replication(node_id);
        Node node(config_with(dir, root_from("owner-token")), &replication);
        ASSERT_TRUE(node.start());
        EXPECT_EQ(node.authenticate_token(secret), nullptr)
            << "the replay written while the node was down was honored";
        EXPECT_TRUE(node.authenticate_token("owner-token"));
        node.stop();
    }
    {
        // A malformed file refuses the start rather than forgetting what it held.
        std::ofstream(dir.path() / "security" / "identity-held.json") << "{not json";
        ReplicationManager replication(node_id);
        Node node(config_with(dir, root_from("owner-token")), &replication);
        EXPECT_FALSE(node.start());
    }
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
