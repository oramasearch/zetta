// The identity epoch (#1360): the root seals one record committing to the
// version of every identity record, so a node that never saw a newer record
// (a revocation) still refuses the older one once the epoch reaches it, even
// when no node that held the newer record is left to write it back. Nodes
// exchange documents with merge(save()), the way gossip does. See
// docs/governance/identity-seal.md ("The identity epoch").
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

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
using zetta::governance::kIdentityEpochKey;
using zetta::test::TempDir;

namespace {

constexpr const char* kOwner = "owner-token-for-the-epoch-tests";

IdentityAuthority root_from(const std::string& secret) {
    auto root = IdentityAuthority::from_owner_secret(secret);
    EXPECT_TRUE(root);
    return *root;
}

NodeConfig config_with(const TempDir& dir, std::optional<IdentityAuthority> root,
                       std::uint64_t min_epoch = 0) {
    NodeConfig config;
    config.data_dir = dir.path().string();
    config.preflight = false;
    config.identity_authority = std::move(root);
    config.identity_min_epoch = min_epoch;
    return config;
}

// Past the epoch's coalescing interval, then a tick: the next epoch is sealed.
void seal_next_epoch(Node& node) {
    std::this_thread::sleep_for(Node::kIdentityEpochInterval + std::chrono::milliseconds(100));
    node.tick();
}

IdentityAuthority verify_only_root() {
    return *IdentityAuthority::verify_only(root_from(kOwner).public_key_hex());
}

}  // namespace

TEST(IdentityEpoch, TheDigestCoversEveryEntry) {
    const nlohmann::json entries{{"token:a", 3}, {"user:b", 7}};
    const nlohmann::json value = zetta::governance::identity_epoch_value(entries, 1);
    zetta::governance::OpenedIdentityRecord epoch;
    epoch.value = value;
    epoch.version = 10;
    const auto committed = zetta::governance::identity_epoch_entries(epoch);
    ASSERT_TRUE(committed);
    EXPECT_EQ(committed->at("token:a"), 3u);
    EXPECT_EQ(committed->at("user:b"), 7u);

    epoch.value["entries"]["token:a"] = 1;  // the digest no longer matches
    EXPECT_FALSE(zetta::governance::identity_epoch_entries(epoch));
    epoch.value = zetta::governance::identity_epoch_value(
        nlohmann::json{{"not-identity:x", 1}}, 1);
    EXPECT_FALSE(zetta::governance::identity_epoch_entries(epoch));
    EXPECT_TRUE(zetta::governance::is_identity_key(kIdentityEpochKey));
}

TEST(IdentityEpoch, TheRootNodeSealsAnEpochAfterAnIdentityWrite) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, root_from(kOwner)), &replication);
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner(kOwner).performed);
    node.tick();
    const auto first = node.identity_epoch_status();
    ASSERT_GT(first.epoch, 0u);
    auto alice = node.create_user("alice");
    ASSERT_TRUE(alice);
    seal_next_epoch(node);
    const auto second = node.identity_epoch_status();
    EXPECT_GT(second.epoch, first.epoch) << "the epoch number only grows";
    EXPECT_GT(second.entries, first.entries);
    const nlohmann::json sealed = replication.snapshot()[std::string(kIdentityEpochKey)];
    const auto opened = node.identity_authority()->open(std::string(kIdentityEpochKey), sealed);
    ASSERT_TRUE(opened);
    const auto committed = zetta::governance::identity_epoch_entries(*opened);
    ASSERT_TRUE(committed);
    EXPECT_TRUE(committed->contains("user:" + alice->id()));
    node.stop();
}

// The issue's case: a token is revoked, every node that held the revocation is
// gone, and a fresh node is offered the pre-revocation token record. With the
// epoch that committed the revocation, it refuses the token.
TEST(IdentityEpoch, AFreshNodeRefusesARecordOlderThanTheEpochCommits) {
    TempDir dir_root;
    ReplicationManager repl_root(zetta::util::new_uuid());
    Node root(config_with(dir_root, root_from(kOwner)), &repl_root);
    ASSERT_TRUE(root.start());
    ASSERT_TRUE(root.bootstrap_owner(kOwner).performed);
    auto alice = root.create_user("alice");
    ASSERT_TRUE(alice);
    const auto issued = root.create_token(alice->id(), zetta::governance::TokenType::Access);
    ASSERT_TRUE(issued.token);
    const std::string token_key = "token:" + issued.token->id();
    const nlohmann::json before = repl_root.snapshot()[token_key];
    root.tick();
    ASSERT_TRUE(root.revoke_token(issued.token->id()));
    seal_next_epoch(root);  // the epoch that commits the revocation
    const std::uint64_t epoch = root.identity_epoch_status().epoch;
    ASSERT_GT(epoch, 0u);

    // A brand-new node that never held the revocation: the document it gets
    // carries the epoch, but the token key holds the pre-revocation record.
    TempDir dir_fresh;
    ReplicationManager repl_fresh(zetta::util::new_uuid());
    Node fresh(config_with(dir_fresh, verify_only_root()), &repl_fresh);
    ASSERT_TRUE(fresh.start());
    repl_fresh.merge(repl_root.save());
    repl_fresh.set(token_key, before);  // the replay, written after the merge
    fresh.tick();
    EXPECT_EQ(fresh.identity_epoch_status().epoch, epoch);
    EXPECT_EQ(fresh.authenticate_token(issued.secret), nullptr)
        << "the pre-revocation token record was honored";
    EXPECT_TRUE(fresh.authenticate_token(kOwner));
    root.stop();
    fresh.stop();
}

TEST(IdentityEpoch, AnOlderEpochIsRefusedAndTheNewerOneWrittenBack) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, root_from(kOwner)), &replication);
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner(kOwner).performed);
    node.tick();
    const std::string key(kIdentityEpochKey);
    const nlohmann::json older = replication.snapshot()[key];
    ASSERT_TRUE(node.create_user("alice"));
    seal_next_epoch(node);
    const nlohmann::json newer = replication.snapshot()[key];
    ASSERT_NE(older, newer);
    replication.set(key, older);
    node.tick();
    EXPECT_EQ(replication.snapshot()[key], newer);
    node.stop();
}

TEST(IdentityEpoch, AForgedEpochIsIgnored) {
    TempDir dir;
    ReplicationManager replication(zetta::util::new_uuid());
    Node node(config_with(dir, root_from(kOwner)), &replication);
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner(kOwner).performed);
    node.tick();
    // Another root's epoch commits the owner's token at an unreachable version.
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& [key, value] : replication.snapshot().items()) {
        if (key.starts_with("token:")) entries[key] = std::uint64_t{1} << 62;
    }
    const IdentityAuthority attacker = root_from("attacker-owner-token");
    const auto forged = attacker.seal(std::string(kIdentityEpochKey),
                                      zetta::governance::identity_epoch_value(entries, 1),
                                      std::uint64_t{1} << 62);
    ASSERT_TRUE(forged);
    replication.set(std::string(kIdentityEpochKey), *forged);
    node.tick();
    EXPECT_TRUE(node.authenticate_token(kOwner));
    node.stop();
}

TEST(IdentityEpoch, AMinimumEpochHoldsBackIdentityUntilItArrives) {
    TempDir dir_root;
    ReplicationManager repl_root(zetta::util::new_uuid());
    Node root(config_with(dir_root, root_from(kOwner)), &repl_root);
    ASSERT_TRUE(root.start());
    ASSERT_TRUE(root.bootstrap_owner(kOwner).performed);
    root.tick();
    const std::uint64_t epoch = root.identity_epoch_status().epoch;
    ASSERT_GT(epoch, 0u);
    {
        // A floor above every epoch the cluster has: nothing is honored.
        TempDir dir;
        ReplicationManager replication(zetta::util::new_uuid());
        Node joiner(config_with(dir, verify_only_root(), epoch + 1'000'000), &replication);
        ASSERT_TRUE(joiner.start());
        replication.merge(repl_root.save());
        joiner.tick();
        EXPECT_FALSE(joiner.identity_epoch_status().floor_met);
        EXPECT_EQ(joiner.authenticate_token(kOwner), nullptr);
        joiner.stop();
    }
    {
        // The floor the operator recorded: the epoch arrives, identity is honored.
        TempDir dir;
        ReplicationManager replication(zetta::util::new_uuid());
        Node joiner(config_with(dir, verify_only_root(), epoch), &replication);
        ASSERT_TRUE(joiner.start());
        replication.merge(repl_root.save());
        joiner.tick();
        EXPECT_TRUE(joiner.identity_epoch_status().floor_met);
        EXPECT_TRUE(joiner.authenticate_token(kOwner));
        joiner.stop();
    }
    root.stop();
}
