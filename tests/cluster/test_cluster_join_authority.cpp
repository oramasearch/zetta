// "Unauthorized node cannot join or orchestrate" (#848), the parts that are
// about authority rather than transport: a node that legitimately joins a
// cluster gets membership and nothing else. It mints no owner (#763), and what
// it writes into the replicated catalogue itself is honored nowhere (#764).
//
// Real in-process clusters over mutual TLS (one join token), each with a real
// Node and its identity root, in the order main.cpp runs them: start, join, wait
// until a peer's document matches, reconcile, then seed identity.
//
// The transport half of #848 (no token, a wrong token, another cluster's token,
// nothing readable on the wire, an authenticated cluster forms and syncs) is in
// test_cluster_security.cpp. A removed node staying removed (#848 step 6) is in
// test_cluster_revocation.cpp (#1320). Coverage map: docs/testing/authz-regressions.md.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "../temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/identity_seal.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::CapabilityDomain;
using zetta::governance::IdentityAuthority;
using zetta::governance::Operation;
using zetta::test::TempDir;

namespace {

constexpr const char* kClusterOwner = "cluster-owner-admin-token";

IdentityAuthority root_of(const std::string& owner_secret) {
    return *IdentityAuthority::from_owner_secret(owner_secret);
}

// One node: data dir, replicated document, Node with its identity root, and a
// Cluster over mutual TLS with the shared join token.
struct Member {
    TempDir dir;
    std::string id = zetta::util::new_uuid();
    ReplicationManager replication{id};
    std::unique_ptr<Node> node;
    std::unique_ptr<Cluster> cluster;

    explicit Member(std::optional<IdentityAuthority> root) {
        NodeConfig config;
        config.data_dir = dir.path().string();
        config.preflight = false;
        config.identity_authority = std::move(root);
        node = std::make_unique<Node>(config, &replication);
        EXPECT_TRUE(node->start());
    }
    ~Member() {
        if (cluster) cluster->stop();
        node->stop();
    }
    void connect(const std::string& join_token, std::uint16_t port,
                 const std::string& seed = {}) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        if (!seed.empty()) config.seeds = {seed};
        std::string reason;
        auto security = zetta::cluster::derive_cluster_security(join_token, id, &reason);
        EXPECT_TRUE(security) << reason;
        config.security = *security;
        cluster = std::make_unique<Cluster>(id, config, &replication);
        cluster->start();
    }
    std::size_t owners() const {
        std::size_t count = 0;
        for (const auto& user : node->users()) count += user->name() == "owner";
        return count;
    }
};

// Tick both nodes until `done` holds (patient: real TCP + TLS in-process).
template <class Predicate>
bool converge(Member& a, Member& b, Predicate done, int timeout_ms = 30000) {
    for (int waited = 0; waited < timeout_ms; waited += 25) {
        a.node->tick();
        b.node->tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

bool in_sync(const Member& a, const Member& b) {
    return a.replication.version() == b.replication.version();
}

// The founder with its owner, and a joiner connected to it and in sync — the
// state main.cpp waits for before it seeds identity on the joiner.
void found_and_join(Member& founder, Member& joiner, std::uint16_t port) {
    ASSERT_TRUE(founder.node->bootstrap_owner(kClusterOwner).performed);
    const std::string token = zetta::cluster::generate_join_token();
    founder.connect(token, port);
    joiner.connect(token, static_cast<std::uint16_t>(port + 1),
                   "127.0.0.1:" + std::to_string(port));
    ASSERT_TRUE(converge(founder, joiner, [&] {
        return joiner.cluster->peer_count() == 1 && in_sync(founder, joiner);
    })) << "the joiner never synced with the founder over mTLS";
}

}  // namespace

// #848 step 4 / #763: a node joins with its own --admin-token and, once synced,
// seeds an owner. It holds a root of its own, so the owner it seeds is sealed by
// that root, and no other node honors it.
TEST(ClusterJoinAuthority, AJoinerWithItsOwnAdminTokenMintsNoOwnerOnTheCluster) {
    Member founder(root_of(kClusterOwner));
    Member joiner(root_of("joiner-own-admin-token"));
    found_and_join(founder, joiner, 43840);

    joiner.node->ensure_builtin_roles();
    joiner.node->bootstrap_owner("joiner-own-admin-token");
    ASSERT_TRUE(converge(founder, joiner, [&] { return in_sync(founder, joiner); }));

    EXPECT_EQ(founder.owners(), 1u);
    EXPECT_EQ(founder.node->authenticate_token("joiner-own-admin-token"), nullptr);
    EXPECT_NE(founder.node->authenticate_token(kClusterOwner), nullptr);
    std::size_t admins = 0;
    for (const auto& role : founder.node->roles()) admins += role->name() == "admin";
    EXPECT_EQ(admins, 1u) << "the joiner's built-in roles reached the founder";
}

// #763, the legitimate case: a joiner given the cluster's admin token seeds
// nothing new. It sees the owner after the sync, and even had it seeded first,
// the ids are fixed by the root, so the cluster would still hold one owner.
TEST(ClusterJoinAuthority, AJoinerWithTheClustersAdminTokenAddsNoSecondOwner) {
    Member founder(root_of(kClusterOwner));
    Member joiner(root_of(kClusterOwner));
    found_and_join(founder, joiner, 43842);

    joiner.node->ensure_builtin_roles();
    const auto boot = joiner.node->bootstrap_owner(kClusterOwner);
    EXPECT_FALSE(boot.performed) << "the joiner re-seeded an owner after syncing";
    ASSERT_TRUE(converge(founder, joiner, [&] { return in_sync(founder, joiner); }));

    EXPECT_EQ(founder.owners(), 1u);
    EXPECT_EQ(joiner.owners(), 1u);
    EXPECT_NE(joiner.node->authenticate_token(kClusterOwner), nullptr);
}

// #848 step 5 / #764: membership is not authority. A member that pinned the
// cluster's root but does not hold it cannot issue identity through its Node,
// and what it writes into the replicated catalogue directly — a new admin
// account, or a widened built-in role — is honored by no node.
TEST(ClusterJoinAuthority, AMemberCannotGrantItselfAuthority) {
    Member founder(root_of(kClusterOwner));
    Member member(IdentityAuthority::verify_only(root_of(kClusterOwner).public_key_hex()));
    found_and_join(founder, member, 43844);
    ASSERT_NE(member.node->authenticate_token(kClusterOwner), nullptr)
        << "a verify-only member must still honor the cluster's real owner";

    // Through the Node: refused.
    EXPECT_FALSE(member.node->create_user("mallory"));
    EXPECT_FALSE(member.node->grant_permissions("reader", {"cluster:admin"}));

    // Straight into the document: a forged admin account and token...
    TempDir forge_dir;
    NodeConfig forge_config;
    forge_config.data_dir = forge_dir.path().string();
    forge_config.preflight = false;
    Node forge(forge_config);
    ASSERT_TRUE(forge.start());
    auto mallory = forge.create_user("mallory");
    ASSERT_TRUE(mallory);
    const auto issued = forge.create_token(mallory->id(), zetta::governance::TokenType::Access);
    ASSERT_TRUE(issued.token);
    const std::string admin_role_id = founder.node->find_role("admin")->id();
    member.replication.set("user:" + mallory->id(), mallory->metadata());
    member.replication.set("user_role:" + zetta::util::new_uuid(),
                           nlohmann::json{{"user", mallory->id()}, {"role", admin_role_id}});
    member.replication.set("token:" + issued.token->id(), issued.token->metadata());
    forge.stop();

    // ...and the sealed reader role, widened to admin rights, seal kept.
    auto reader = founder.node->find_role("reader");
    ASSERT_TRUE(reader);
    nlohmann::json widened =
        member.replication.snapshot()["role:" + reader->id()];
    widened["capabilities"] = founder.node->find_role("admin")->metadata()["capabilities"];
    member.replication.set("role:" + reader->id(), widened);

    ASSERT_TRUE(converge(founder, member, [&] { return in_sync(founder, member); }));

    for (Member* node : {&founder, &member}) {
        EXPECT_EQ(node->node->authenticate_token(issued.secret), nullptr);
        EXPECT_FALSE(node->node->find_user("mallory"));
        auto role = node->node->find_role_by_id(reader->id());
        // The widened role is not honored: the node keeps the sealed reader role
        // it holds (#1341), never the widened one.
        if (role) {
            EXPECT_FALSE(role->capabilities().permits(
                Operation{CapabilityDomain::Api, "cluster_admin"}, "2026-10-21T00:00:00Z"));
        }
        EXPECT_NE(node->node->authenticate_token(kClusterOwner), nullptr);
    }
}

// #1341, replay: a member that kept an earlier sealed value of a record writes
// it back after the owner changed it. The old value still carries a valid seal,
// so before the seal bound a version, it opened and undid the change. Two
// changes are replayed: a token before its revocation, and a role assignment
// before it was removed. Every node keeps the newer value, and the document is
// put back to it, so a node that joins later sees the revocation too.
TEST(ClusterJoinAuthority, AMemberCannotUndoARevocationByReplayingAnOldSealedRecord) {
    Member founder(root_of(kClusterOwner));
    Member member(IdentityAuthority::verify_only(root_of(kClusterOwner).public_key_hex()));
    found_and_join(founder, member, 43846);

    auto alice = founder.node->create_user("alice");
    ASSERT_TRUE(alice);
    const auto issued =
        founder.node->create_token(alice->id(), zetta::governance::TokenType::Access);
    ASSERT_TRUE(issued.token);
    auto admin = founder.node->find_role("admin");
    ASSERT_TRUE(admin);
    auto assignment = founder.node->assign_role(alice->id(), admin->id());
    ASSERT_TRUE(assignment);
    const std::string token_key = "token:" + issued.token->id();
    const std::string assignment_key = "user_role:" + assignment->id();
    ASSERT_TRUE(converge(founder, member, [&] {
        return member.node->authenticate_token(issued.secret) != nullptr &&
               member.node->user_roles().size() == founder.node->user_roles().size();
    }));

    // The member keeps the sealed values it holds now...
    nlohmann::json snapshot = member.replication.snapshot();
    const nlohmann::json old_token = snapshot[token_key];
    const nlohmann::json old_assignment = snapshot[assignment_key];

    // ...the owner revokes the token and removes the assignment...
    ASSERT_TRUE(founder.node->revoke_token(issued.token->id()));
    ASSERT_TRUE(founder.node->unassign_role(alice->id(), admin->id()));
    const auto has_assignment = [&](Member& m) {
        for (const auto& held : m.node->user_roles()) {
            if (held->id() == assignment->id()) return true;
        }
        return false;
    };
    ASSERT_TRUE(converge(founder, member, [&] {
        return member.node->authenticate_token(issued.secret) == nullptr &&
               !has_assignment(member);
    })) << "the revocation never reached the member";

    // ...and the member writes the old sealed values back.
    member.replication.set(token_key, old_token);
    member.replication.set(assignment_key, old_assignment);
    // Give the replay time to reach the founder and every node to reconcile it.
    converge(founder, member, [] { return false; }, 2000);
    ASSERT_TRUE(converge(founder, member, [&] { return in_sync(founder, member); }));

    for (Member* m : {&founder, &member}) {
        EXPECT_EQ(m->node->authenticate_token(issued.secret), nullptr)
            << "a replayed sealed token undid its revocation";
        EXPECT_FALSE(has_assignment(*m))
            << "a replayed sealed assignment undid its removal";
        EXPECT_NE(m->node->authenticate_token(kClusterOwner), nullptr);
    }
    // The document holds the revoked token again, not the replayed one.
    nlohmann::json now = member.replication.snapshot();
    ASSERT_TRUE(now.contains(token_key));
    EXPECT_EQ(now[token_key].value("revoked", false), true);
    EXPECT_NE(now[assignment_key], old_assignment);
}

// #1341, removals: a removal is a sealed tombstone issued by the root. A member
// that deletes identity records straight from the document (the CRDT's own
// unsealed tombstone) or writes a tombstone-shaped value of its own deletes
// nothing anyone honors.
TEST(ClusterJoinAuthority, AMemberCannotDeleteIdentityWithAnUnsealedRemoval) {
    Member founder(root_of(kClusterOwner));
    Member member(IdentityAuthority::verify_only(root_of(kClusterOwner).public_key_hex()));
    found_and_join(founder, member, 43848);

    auto alice = founder.node->create_user("alice");
    ASSERT_TRUE(alice);
    const auto issued =
        founder.node->create_token(alice->id(), zetta::governance::TokenType::Access);
    ASSERT_TRUE(issued.token);
    auto reader = founder.node->find_role("reader");
    ASSERT_TRUE(reader);
    ASSERT_TRUE(founder.node->assign_role(alice->id(), reader->id()));
    ASSERT_TRUE(converge(founder, member, [&] {
        return member.node->authenticate_token(issued.secret) != nullptr;
    }));
    const std::size_t assignments = founder.node->user_roles().size();

    // Through the Node: refused, as every identity write is on a verify-only node.
    EXPECT_FALSE(member.node->revoke_token(issued.token->id()));
    EXPECT_FALSE(member.node->drop_user("alice"));

    // Straight into the document.
    member.replication.remove("token:" + issued.token->id());
    member.replication.remove("user:" + alice->id());
    member.replication.set("role:" + reader->id(),
                           nlohmann::json{{"_tombstone", true}, {"_v", 1u << 30}});
    for (const auto& held : member.node->user_roles()) {
        member.replication.remove("user_role:" + held->id());
    }
    converge(founder, member, [] { return false; }, 2000);
    ASSERT_TRUE(converge(founder, member, [&] { return in_sync(founder, member); }));

    for (Member* m : {&founder, &member}) {
        EXPECT_NE(m->node->authenticate_token(issued.secret), nullptr)
            << "an unsealed removal deleted a token";
        EXPECT_TRUE(m->node->find_user("alice")) << "an unsealed removal deleted a user";
        EXPECT_TRUE(m->node->find_role_by_id(reader->id()))
            << "a forged tombstone deleted a role";
        EXPECT_EQ(m->node->user_roles().size(), assignments)
            << "an unsealed removal deleted a role assignment";
        EXPECT_NE(m->node->authenticate_token(kClusterOwner), nullptr);
    }

    // A removal the owner makes is sealed, and it reaches every node.
    ASSERT_TRUE(founder.node->revoke_token(issued.token->id()));
    ASSERT_TRUE(founder.node->drop_user("alice"));
    ASSERT_TRUE(converge(founder, member, [&] {
        return !member.node->find_user("alice") &&
               member.node->authenticate_token(issued.secret) == nullptr;
    })) << "a sealed removal did not reach the member";
}
