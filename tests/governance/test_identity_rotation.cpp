// Rotating the identity root by succession (#1342): the current root seals a
// record naming its successor, the rotating node re-seals every identity record
// under the new root, and every other node moves its pin when the succession
// reaches it. Two nodes exchange their documents with merge(save()), the way
// gossip does. See docs/governance/identity-seal.md ("Rotating the root").
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/identity_seal.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::IdentityAuthority;
using zetta::governance::IdentityRootSuccession;
using zetta::governance::kIdentitySuccessionPrefix;
using zetta::governance::load_or_create_identity_root;
using zetta::test::TempDir;

namespace {

constexpr const char* kOldToken = "owner-token-before-rotation";
constexpr const char* kNewToken = "owner-token-after-the-rotation";

IdentityAuthority root_from(const std::string& secret) {
    auto root = IdentityAuthority::from_owner_secret(secret);
    EXPECT_TRUE(root);
    return *root;
}

NodeConfig config_with(const TempDir& dir, std::optional<IdentityAuthority> root,
                       std::optional<IdentityAuthority> candidate = std::nullopt) {
    NodeConfig config;
    config.data_dir = dir.path().string();
    config.preflight = false;
    config.identity_authority = std::move(root);
    config.identity_owner_candidate = std::move(candidate);
    return config;
}

// Gossip, both ways, then let each node reconcile what arrived.
void exchange(ReplicationManager& a, Node& node_a, ReplicationManager& b, Node& node_b) {
    a.merge(b.save());
    b.merge(a.save());
    node_a.tick();
    node_b.tick();
}

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    return nlohmann::json::parse(in, nullptr, false);
}

}  // namespace

// ----- the succession record ---------------------------------------------------

TEST(IdentitySuccession, OnlyTheOldRootCanSealASuccession) {
    const IdentityAuthority old_root = root_from(kOldToken);
    const IdentityAuthority new_root = root_from(kNewToken);
    const std::string key = std::string(kIdentitySuccessionPrefix) + new_root.public_key_hex();
    const auto sealed = old_root.seal_succession(new_root, 1'000);
    ASSERT_TRUE(sealed);
    const auto opened = zetta::governance::open_identity_succession(key, *sealed);
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->old_root, old_root.public_key_hex());
    EXPECT_EQ(opened->new_root, new_root.public_key_hex());
    EXPECT_EQ(opened->issued_at, 1'000u);

    // A forged succession: sealed by the new root itself, naming the old one.
    const IdentityAuthority attacker = root_from("someone-else");
    auto forged = attacker.seal_succession(new_root, 1'000);
    ASSERT_TRUE(forged);
    (*forged)["old"] = old_root.public_key_hex();
    EXPECT_FALSE(zetta::governance::open_identity_succession(key, *forged));

    // Moved to another key: refused, the key names the new root.
    const std::string other =
        std::string(kIdentitySuccessionPrefix) + attacker.public_key_hex();
    EXPECT_FALSE(zetta::governance::open_identity_succession(other, *sealed));

    // A verify-only root seals nothing, and no root succeeds itself.
    EXPECT_FALSE(IdentityAuthority::verify_only(old_root.public_key_hex())
                     ->seal_succession(new_root, 1'000));
    EXPECT_FALSE(old_root.seal_succession(old_root, 1'000));
}

TEST(IdentitySuccession, TheChainIsFollowedFromThePinnedRoot) {
    const IdentityAuthority a = root_from("a");
    const IdentityAuthority b = root_from("b");
    const IdentityAuthority c = root_from("c");
    nlohmann::json document = nlohmann::json::object();
    document[std::string(kIdentitySuccessionPrefix) + b.public_key_hex()] =
        *a.seal_succession(b, 1);
    document[std::string(kIdentitySuccessionPrefix) + c.public_key_hex()] =
        *b.seal_succession(c, 2);
    const auto from_a = zetta::governance::identity_succession_chain(a.public_key_hex(), document);
    ASSERT_EQ(from_a.size(), 2u);
    EXPECT_EQ(from_a.back().new_root, c.public_key_hex());
    const auto from_b = zetta::governance::identity_succession_chain(b.public_key_hex(), document);
    ASSERT_EQ(from_b.size(), 1u);
    EXPECT_TRUE(
        zetta::governance::identity_succession_chain(c.public_key_hex(), document).empty());
}

TEST(IdentitySuccession, ThePinKeepsTheRootsItLeft) {
    TempDir dir;
    const IdentityAuthority old_root = root_from(kOldToken);
    const IdentityAuthority new_root = root_from(kNewToken);
    ASSERT_TRUE(load_or_create_identity_root(dir.path().string(), kOldToken, "").authority);
    ASSERT_TRUE(zetta::governance::repin_identity_root(dir.path().string(), new_root,
                                                       {old_root.public_key_hex()}));
    const nlohmann::json pin = read_json(dir.path() / "security" / "identity-root.json");
    EXPECT_EQ(pin["public_key"], new_root.public_key_hex());
    EXPECT_EQ(pin["previous"], nlohmann::json::array({old_root.public_key_hex()}));
    // Restarted with the new token: the pinned root is the token's, with the seed.
    const auto setup = load_or_create_identity_root(dir.path().string(), kNewToken, "");
    ASSERT_TRUE(setup.authority);
    EXPECT_TRUE(setup.authority->can_seal());
    EXPECT_EQ(setup.previous_roots, std::vector<std::string>{old_root.public_key_hex()});
    EXPECT_FALSE(setup.owner_candidate);
}

TEST(IdentitySuccession, ATokenThatIsNotThePinIsACandidate) {
    TempDir dir;
    ASSERT_TRUE(load_or_create_identity_root(dir.path().string(), kOldToken, "").authority);
    const auto setup = load_or_create_identity_root(dir.path().string(), kNewToken, "");
    ASSERT_TRUE(setup.authority);
    ASSERT_TRUE(setup.owner_candidate);
    EXPECT_EQ(setup.owner_candidate->public_key_hex(), root_from(kNewToken).public_key_hex());
    EXPECT_NE(setup.warning.find("rotate_identity_root"), std::string::npos) << setup.warning;
}

// ----- rotating a running cluster --------------------------------------------------

TEST(IdentityRotation, TheOtherNodeMovesItsPinAndHonorsTheNewRoot) {
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a(zetta::util::new_uuid());
    ReplicationManager repl_b(zetta::util::new_uuid());
    // A holds the root; B verifies only.
    Node a(config_with(dir_a, root_from(kOldToken)), &repl_a);
    Node b(config_with(dir_b, IdentityAuthority::verify_only(
                                  root_from(kOldToken).public_key_hex())),
           &repl_b);
    ASSERT_TRUE(a.start());
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(a.bootstrap_owner(kOldToken).performed);
    auto alice = a.create_user("alice");
    ASSERT_TRUE(alice);
    exchange(repl_a, a, repl_b, b);
    ASSERT_TRUE(b.find_user("alice"));

    const Node::IdentityRotationResult rotation = a.rotate_identity_root(kNewToken, false);
    ASSERT_TRUE(rotation.ok) << rotation.reason;
    EXPECT_EQ(rotation.new_root, root_from(kNewToken).public_key_hex());
    EXPECT_GT(rotation.resealed, 0u);
    EXPECT_FALSE(rotation.owner_token_id.empty());
    EXPECT_EQ(a.identity_authority()->public_key_hex(), rotation.new_root);
    EXPECT_TRUE(a.authenticate_token(kNewToken));

    exchange(repl_a, a, repl_b, b);
    EXPECT_EQ(b.identity_authority()->public_key_hex(), rotation.new_root);
    EXPECT_FALSE(b.can_issue_identity());
    // B accepts a record newly sealed by the new root.
    auto bob = a.create_user("bob");
    ASSERT_TRUE(bob);
    exchange(repl_a, a, repl_b, b);
    EXPECT_TRUE(b.find_user("bob"));
    EXPECT_TRUE(b.find_user("alice"));
    EXPECT_TRUE(b.authenticate_token(kNewToken));
    // B re-pinned, keeping the old root as history.
    const nlohmann::json pin = read_json(dir_b.path() / "security" / "identity-root.json");
    EXPECT_EQ(pin["public_key"], rotation.new_root);
    EXPECT_EQ(pin["previous"], nlohmann::json::array({rotation.old_root}));
    a.stop();
    b.stop();
}

TEST(IdentityRotation, ARecordTheOldRootSealsAfterTheMoveIsRefused) {
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a(zetta::util::new_uuid());
    ReplicationManager repl_b(zetta::util::new_uuid());
    Node a(config_with(dir_a, root_from(kOldToken)), &repl_a);
    Node b(config_with(dir_b, IdentityAuthority::verify_only(
                                  root_from(kOldToken).public_key_hex())),
           &repl_b);
    ASSERT_TRUE(a.start());
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(a.bootstrap_owner(kOldToken).performed);
    ASSERT_TRUE(a.rotate_identity_root(kNewToken, false).ok);
    exchange(repl_a, a, repl_b, b);
    ASSERT_EQ(b.identity_authority()->public_key_hex(), root_from(kNewToken).public_key_hex());

    // Whoever still holds the old root's key seals a new account with it.
    const IdentityAuthority old_root = root_from(kOldToken);
    const std::string key = "user:" + zetta::util::new_uuid();
    const auto forged = old_root.seal(
        key, nlohmann::json{{"name", "mallory"}, {"status", "active"},
                            {"created", "2026-11-14T00:00:00Z"}},
        1);
    ASSERT_TRUE(forged);
    repl_b.set(key, *forged);
    b.tick();
    EXPECT_FALSE(b.find_user("mallory"));
    a.stop();
    b.stop();
}

TEST(IdentityRotation, AForgedSuccessionIsIgnored) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, IdentityAuthority::verify_only(
                                   root_from(kOldToken).public_key_hex())),
              &replication);
    ASSERT_TRUE(node.start());
    const IdentityAuthority attacker = root_from("attacker-token");
    const IdentityAuthority accomplice = root_from("accomplice-token");
    // Sealed by a root the node does not trust, claiming to leave the pinned one.
    auto forged = attacker.seal_succession(accomplice, 1);
    ASSERT_TRUE(forged);
    (*forged)["old"] = root_from(kOldToken).public_key_hex();
    replication.set(std::string(kIdentitySuccessionPrefix) + accomplice.public_key_hex(),
                    *forged);
    node.tick();
    EXPECT_EQ(node.identity_authority()->public_key_hex(),
              root_from(kOldToken).public_key_hex());
    node.stop();
}

TEST(IdentityRotation, ANodeRestartedWithTheNewTokenIssuesIdentity) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_b = zetta::util::new_uuid();
    ReplicationManager repl_a(zetta::util::new_uuid());
    Node a(config_with(dir_a, root_from(kOldToken)), &repl_a);
    ASSERT_TRUE(a.start());
    ASSERT_TRUE(a.bootstrap_owner(kOldToken).performed);
    {
        // B starts under the old root, then stops before the rotation.
        ReplicationManager repl_b(id_b);
        ASSERT_TRUE(load_or_create_identity_root(dir_b.path().string(), kOldToken, "").authority);
        Node b(config_with(dir_b, root_from(kOldToken)), &repl_b);
        ASSERT_TRUE(b.start());
        repl_b.merge(repl_a.save());
        b.tick();
        b.stop();
    }
    ASSERT_TRUE(a.rotate_identity_root(kNewToken, false).ok);
    {
        // Restarted with the new admin token: the pin is still the old root, the
        // token's root is the candidate, and the succession makes it writable.
        const auto setup = load_or_create_identity_root(dir_b.path().string(), kNewToken, "");
        ASSERT_TRUE(setup.authority);
        ASSERT_TRUE(setup.owner_candidate);
        ReplicationManager repl_b(id_b);
        Node b(config_with(dir_b, setup.authority, setup.owner_candidate), &repl_b);
        ASSERT_TRUE(b.start());
        repl_b.merge(repl_a.save());
        b.tick();
        EXPECT_EQ(b.identity_authority()->public_key_hex(), root_from(kNewToken).public_key_hex());
        EXPECT_TRUE(b.can_issue_identity());
        EXPECT_TRUE(b.create_user("carol"));
        b.stop();
    }
    a.stop();
}

TEST(IdentityRotation, RevokeTokensRemovesEveryOtherToken) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, root_from(kOldToken)), &replication);
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner(kOldToken).performed);
    auto alice = node.create_user("alice");
    ASSERT_TRUE(alice);
    const auto issued = node.create_token(alice->id(), zetta::governance::TokenType::Access);
    ASSERT_TRUE(issued.token);
    const auto rotation = node.rotate_identity_root(kNewToken, true);
    ASSERT_TRUE(rotation.ok) << rotation.reason;
    EXPECT_GE(rotation.revoked, 2u);  // the owner's old token and alice's
    node.tick();
    EXPECT_EQ(node.authenticate_token(issued.secret), nullptr);
    EXPECT_EQ(node.authenticate_token(kOldToken), nullptr);
    EXPECT_TRUE(node.authenticate_token(kNewToken));
    node.stop();
}

TEST(IdentityRotation, AVerifyOnlyNodeCannotRotate) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, IdentityAuthority::verify_only(
                                   root_from(kOldToken).public_key_hex())),
              &replication);
    ASSERT_TRUE(node.start());
    const auto rotation = node.rotate_identity_root(kNewToken, false);
    EXPECT_FALSE(rotation.ok);
    EXPECT_FALSE(rotation.reason.empty());
    node.stop();
}
