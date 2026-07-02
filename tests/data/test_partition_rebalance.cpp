#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cloud/add_nodes.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_rebalance.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/query/service.hpp"
#include "zetta/data/rendezvous_placement.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/replication_strategy.hpp"
#include "zetta/data/table.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Database;
using zetta::DatasetPartition;
using zetta::Node;
using zetta::NodeConfig;
using zetta::RebalanceResult;
using zetta::RendezvousPlacement;
using zetta::ReplicatedBlobStore;
using zetta::ReplicationManager;
using zetta::ReplicationStrategy;
using zetta::SinglePartitioner;
using zetta::Table;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A replicated node + a partitioned table holding `count` rows. The table inherits
// the Partitioned replication strategy; the operative signal is the Partitioner the
// test sets. Replication is on so write_partitioned captures lo:/part: into the
// document (the substrate the blob-movement test reads).
struct Fixture {
    Fixture(const TempDir& tmp, std::size_t count)
        : node_id(zetta::new_uuid()), replication(node_id) {
        node = std::make_unique<Node>(config_for(tmp), &replication);
        node->start();
        database = node->create_database("warehouse");
        table = database->create_table("events",
                                       ReplicationStrategy::partitioned());
        for (std::size_t i = 0; i < count; ++i) {
            table->add_row(nlohmann::json{{"id", "row-" + std::to_string(i)},
                                          {"n", static_cast<int>(i)}});
        }
    }

    std::string node_id;
    ReplicationManager replication;
    std::unique_ptr<Node> node;
    std::shared_ptr<Database> database;
    std::shared_ptr<Table> table;
};

std::set<std::string> as_set(const std::vector<std::string>& items) {
    return std::set<std::string>(items.begin(), items.end());
}

// Snapshot every partition's owners, keyed by the catalog row id.
std::map<std::string, std::vector<std::string>> owners_by_id(const Node& node) {
    std::map<std::string, std::vector<std::string>> owners;
    for (const auto& partition : node.dataset_partitions()) {
        owners[partition->id()] = partition->owner_nodes();
    }
    return owners;
}

// Assert the partition row ranges tile [0, total) with no gaps or overlaps. The
// catalog returns partitions sorted by (created, id), not by range, so sort the
// ranges by start first. Rebalancing never touches row_range, so this proves
// "every row remains readable exactly once" survives a re-placement.
void expect_ranges_tile(const Node& node, std::uint64_t total) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto& partition : node.dataset_partitions()) {
        ranges.emplace_back(partition->row_range().start,
                            partition->row_range().end);
    }
    std::sort(ranges.begin(), ranges.end());
    std::uint64_t expected_start = 0;
    for (const auto& [start, end] : ranges) {
        EXPECT_EQ(start, expected_start) << "ranges must be contiguous";
        EXPECT_GT(end, start) << "a recorded partition holds at least one row";
        expected_start = end;
    }
    EXPECT_EQ(expected_start, total) << "ranges must cover the whole table";
}

// A minimal fake Provisioner that brings up a fixed list of member ids (capped by
// the requested count, so a count of 0 yields nothing). teardown is COUNTED so the
// test can prove the add_nodes wiring never tears a member down. The add_nodes
// control action programs against the abstract Provisioner&, so this stands in for
// any cloud-burst backend.
class FakeAddProvisioner : public zetta::Provisioner {
public:
    explicit FakeAddProvisioner(std::vector<std::string> new_ids)
        : new_ids_(std::move(new_ids)) {}

    int teardown_calls = 0;

    zetta::ProvisionResult provision(
        const zetta::ProvisionRequest& request) override {
        zetta::ProvisionResult result;
        const std::size_t target = std::min(request.count, new_ids_.size());
        for (std::size_t index = 0; index < target; ++index) {
            zetta::ProvisionedNode node;
            node.node_id = new_ids_[index];
            node.address = new_ids_[index] + ":7777";
            node.capability = request.spec;
            result.nodes.push_back(std::move(node));
        }
        result.satisfied = result.nodes.size() >= request.count;
        result.detail = result.satisfied ? "added" : "best-effort short";
        return result;
    }

    bool teardown(const std::string&) override {
        ++teardown_calls;
        return false;
    }

    std::vector<zetta::ProvisionedNode> provisioned() const override { return {}; }

private:
    std::vector<std::string> new_ids_;
};

}  // namespace

// ---- the acceptance: adding a node moves only ~1/N partitions onto it ----------

TEST(PartitionRebalance, RendezvousMovesAboutOneOverNToNewNodeRfOne) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/60};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/64, /*replication_factor=*/1));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const auto write = zetta::write_partitioned(*fx.node, *fx.database, *fx.table,
                                                members);
    ASSERT_TRUE(write.ok()) << write.error;
    ASSERT_GT(write.partitions.size(), 3u);  // a spread of distinct partitions
    for (const auto& partition : fx.node->dataset_partitions()) {
        ASSERT_EQ(partition->owner_nodes().size(), 1u);  // rf=1 at write time
    }
    const auto before = owners_by_id(*fx.node);

    const std::vector<std::string> grown{"node-a", "node-b", "node-c", "node-d"};
    const RebalanceResult result =
        zetta::rebalance_partitions(*fx.node, grown, members);

    EXPECT_EQ(result.examined, fx.node->dataset_partition_count());
    EXPECT_GT(result.moved, 0u);                  // adding a node moves some
    EXPECT_LT(result.moved, result.examined);     // but NOT a full rehash (the HRW win)

    // Each move handed the partition to the new node; nothing else changed.
    for (const zetta::PartitionMove& move : result.moves) {
        ASSERT_EQ(move.to.size(), 1u);
        EXPECT_EQ(move.to.front(), "node-d");
    }
    std::size_t owned_by_d = 0;
    for (const auto& partition : fx.node->dataset_partitions()) {
        ASSERT_EQ(partition->owner_nodes().size(), 1u);  // rf preserved
        if (partition->owner_nodes().front() == "node-d") {
            ++owned_by_d;
        } else {
            // An untouched partition keeps its exact original owner — HRW only
            // reassigns a partition the new node wins.
            EXPECT_EQ(partition->owner_nodes(), before.at(partition->id()));
        }
    }
    EXPECT_EQ(owned_by_d, result.moved);
    EXPECT_LT(owned_by_d, fx.node->dataset_partition_count());

    // Every row is still covered exactly once.
    expect_ranges_tile(*fx.node, write.rows_written);

    // Determinism: re-running with the now-current membership moves nothing.
    const RebalanceResult again =
        zetta::rebalance_partitions(*fx.node, grown, grown);
    EXPECT_EQ(again.moved, 0u);
}

// ---- replicas: rf is preserved, every row still readable rf times ---------------

TEST(PartitionRebalance, ReplicationFactorPreservedAcrossRebalance) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/40};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/32, /*replication_factor=*/2));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const auto write = zetta::write_partitioned(*fx.node, *fx.database, *fx.table,
                                                members);
    ASSERT_TRUE(write.ok()) << write.error;
    ASSERT_GT(write.partitions.size(), 1u);

    const std::vector<std::string> grown{"node-a", "node-b", "node-c", "node-d"};
    const RebalanceResult result =
        zetta::rebalance_partitions(*fx.node, grown, members);
    EXPECT_GT(result.moved, 0u);

    const std::set<std::string> grown_set = as_set(grown);
    std::size_t with_d = 0;
    for (const auto& partition : fx.node->dataset_partitions()) {
        // Every partition still has exactly 2 distinct owners, all live members.
        ASSERT_EQ(partition->owner_nodes().size(), 2u);
        const std::set<std::string> owner_set = as_set(partition->owner_nodes());
        EXPECT_EQ(owner_set.size(), 2u) << "owners are distinct";
        for (const auto& owner : owner_set) {
            EXPECT_TRUE(grown_set.count(owner)) << owner << " is a live member";
        }
        if (owner_set.count("node-d")) ++with_d;
    }
    EXPECT_GT(with_d, 0u);  // the new node took a share of the replicas
    expect_ranges_tile(*fx.node, write.rows_written);
}

// ---- blobs move: the new owner now fetches exactly the partitions it owns -------

TEST(PartitionRebalance, MovedPartitionBodiesFetchByNewOwner) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/40};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/32, /*replication_factor=*/2));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    ASSERT_TRUE(
        zetta::write_partitioned(*fx.node, *fx.database, *fx.table, members).ok());

    const std::vector<std::string> grown{"node-a", "node-b", "node-c", "node-d"};
    const RebalanceResult result =
        zetta::rebalance_partitions(*fx.node, grown, members);
    ASSERT_GT(result.moved, 0u);

    // The bodies node-d now owns (its catalog share).
    std::set<std::string> owned_by_d;
    for (const auto& partition : fx.node->dataset_partitions()) {
        const auto& owners = partition->owner_nodes();
        if (std::find(owners.begin(), owners.end(), "node-d") != owners.end()) {
            owned_by_d.insert(partition->blob_ref());
        }
    }
    ASSERT_FALSE(owned_by_d.empty());

    // node-d's view of the document over a fresh (empty) data dir: it is missing
    // exactly the partition bodies it now owns — i.e. it will LOREQ exactly those,
    // and nothing it doesn't own. This is how the moved blobs actually transfer
    // (ReplicatedBlobStore reads owner_nodes; #515).
    ReplicationManager node_d_view{"node-d"};
    const auto blob = fx.replication.save();
    node_d_view.merge(blob);
    TempDir node_d_dir;
    ReplicatedBlobStore node_d_blobs{node_d_dir.path(), node_d_view};
    const auto missing = node_d_blobs.missing_blobs();
    EXPECT_EQ(as_set(missing), owned_by_d);
}

// ---- replicate-to-all partitions are left untouched -----------------------------

TEST(PartitionRebalance, ReplicateToAllPartitionsAreSkipped) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/6};
    fx.table->set_partitioner(std::make_shared<SinglePartitioner>());

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const auto write = zetta::write_partitioned(*fx.node, *fx.database, *fx.table,
                                                members);
    ASSERT_TRUE(write.ok()) << write.error;
    ASSERT_EQ(fx.node->dataset_partition_count(), 1u);
    EXPECT_EQ(fx.node->dataset_partitions().front()->owner_nodes(), members);

    const std::vector<std::string> grown{"node-a", "node-b", "node-c", "node-d"};
    const RebalanceResult result =
        zetta::rebalance_partitions(*fx.node, grown, members);
    EXPECT_EQ(result.moved, 0u);
    EXPECT_GE(result.skipped, 1u);
    // A replicate-to-all partition is unchanged (sharded rebalance is not its job).
    EXPECT_EQ(fx.node->dataset_partitions().front()->owner_nodes(), members);
}

// ---- the auto-trigger: a membership delta in the document drives a rebalance ----

TEST(PartitionRebalance, TickRebalancesWhenAMemberJoins) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/40};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/32, /*replication_factor=*/1));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    ASSERT_TRUE(
        zetta::write_partitioned(*fx.node, *fx.database, *fx.table, members).ok());

    // Membership lives in the document as node:<id> keys (each node seeds its own;
    // peers merge). Seed the write-time members, then tick to set the baseline —
    // the first observation never rebalances.
    for (const auto& member : members) {
        fx.replication.set("node:" + member,
                           nlohmann::json{{"address", member + ":7777"}});
    }
    fx.node->tick();
    const auto before = owners_by_id(*fx.node);

    // A new node joins: its node:<id> key merges into the document.
    fx.replication.set("node:node-d",
                       nlohmann::json{{"address", "node-d:7777"}});
    fx.node->tick();  // membership delta → auto-rebalance between jobs

    std::size_t owned_by_d = 0;
    bool any_changed = false;
    for (const auto& partition : fx.node->dataset_partitions()) {
        ASSERT_EQ(partition->owner_nodes().size(), 1u);
        if (partition->owner_nodes().front() == "node-d") ++owned_by_d;
        if (partition->owner_nodes() != before.at(partition->id())) {
            any_changed = true;
        }
    }
    EXPECT_GT(owned_by_d, 0u) << "the new member took a share of the partitions";
    EXPECT_LT(owned_by_d, fx.node->dataset_partition_count());
    EXPECT_TRUE(any_changed);

    // Idempotent: another tick with no further membership change moves nothing.
    const auto stable = owners_by_id(*fx.node);
    fx.node->tick();
    EXPECT_EQ(owners_by_id(*fx.node), stable);
}

// ---- #519: add_nodes wires the membership change to the rebalance pass -----------

TEST(PartitionRebalance, AddNodesRebalancesOntoNewMembers) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/60};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/64, /*replication_factor=*/1));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const auto write = zetta::write_partitioned(*fx.node, *fx.database, *fx.table,
                                                members);
    ASSERT_TRUE(write.ok()) << write.error;
    ASSERT_GT(write.partitions.size(), 3u);

    // The current cluster membership lives as node:<id> keys in the document (each
    // node seeds its own; peers merge) — what cluster_members() reads.
    for (const auto& member : members) {
        fx.replication.set("node:" + member,
                           nlohmann::json{{"address", member + ":7777"}});
    }
    ASSERT_EQ(as_set(fx.node->cluster_members()), as_set(members));
    const auto before = owners_by_id(*fx.node);

    // add_nodes brings up one permanent member and rebalances partitions onto it —
    // the new member's id is taken from the provision result.
    FakeAddProvisioner provider{/*new_ids=*/{"node-d"}};
    const zetta::AddNodesResult result = zetta::add_nodes(*fx.node, provider, 1);

    ASSERT_TRUE(result.satisfied);
    ASSERT_EQ(result.members.size(), 1u);
    EXPECT_EQ(result.members.front().node_id, "node-d");
    EXPECT_EQ(provider.teardown_calls, 0);  // a permanent add never tears down

    // The acceptance: the catalog (system.dataset_partitions) now spreads onto the
    // new member — some partitions moved onto it, but not all (the HRW win), and
    // every other partition kept its exact original owner.
    std::size_t owned_by_d = 0;
    bool any_changed = false;
    for (const auto& partition : fx.node->dataset_partitions()) {
        ASSERT_EQ(partition->owner_nodes().size(), 1u);  // rf preserved
        if (partition->owner_nodes().front() == "node-d") {
            ++owned_by_d;
        } else {
            EXPECT_EQ(partition->owner_nodes(), before.at(partition->id()));
        }
        if (partition->owner_nodes() != before.at(partition->id())) {
            any_changed = true;
        }
    }
    EXPECT_GT(owned_by_d, 0u) << "the new member took a share of the partitions";
    EXPECT_LT(owned_by_d, fx.node->dataset_partition_count());
    EXPECT_TRUE(any_changed);

    // Every row is still covered exactly once after the re-placement.
    expect_ranges_tile(*fx.node, write.rows_written);
}

// A best-effort-short / zero add brings up no new member, so there is no membership
// delta and the catalog is left untouched (the rebalance is skipped).
TEST(PartitionRebalance, AddNodesWithNoNewMemberLeavesPlacement) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/40};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/32, /*replication_factor=*/1));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    ASSERT_TRUE(
        zetta::write_partitioned(*fx.node, *fx.database, *fx.table, members).ok());
    for (const auto& member : members) {
        fx.replication.set("node:" + member,
                           nlohmann::json{{"address", member + ":7777"}});
    }
    const auto before = owners_by_id(*fx.node);

    // The provisioner brings up nothing (a zero count, or a backend that came up
    // short) → no new member → no rebalance.
    FakeAddProvisioner provider{/*new_ids=*/{}};
    const zetta::AddNodesResult result = zetta::add_nodes(*fx.node, provider, 0);
    EXPECT_TRUE(result.members.empty());

    EXPECT_EQ(owners_by_id(*fx.node), before);  // placement unchanged
}

// ---- TC-10 (#500): the catalog reflects the new placement on EVERY node ---------
//
// The single-node tests above prove the rebalance *mechanism*; TC-10's make-or-break
// clause is that, in a real cluster, after a node joins the
// `system.dataset_partitions` catalog converges to the *identical* new placement on
// **every** node — partitions replicate through the CRDT, so a re-placement captured
// on the writer reconciles onto its peers. Two replicated nodes here: the writer (A)
// partitions a table and drives the join via the production tick auto-trigger; the
// peer (B) inherits the rebalanced catalog by merging A's document and ticking. We
// then assert both nodes' SQL views agree, the new node took ~1/N, the replication
// factor held, and the row ranges still tile [0, total) on both.

namespace {

// A node's placement read through the queryable `system.dataset_partitions` view —
// the catalog as the product exposes it. Keyed by the catalog row id (`part:<uuid>`,
// stable across nodes since it is the CRDT key), so two nodes' maps compare directly.
struct SqlPlacement {
    std::map<std::string, std::string> owners_by_part;  // part id -> "n1, n2"
    std::map<std::string, int> rf_by_part;              // part id -> owner_count
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;  // [start, end)
    std::size_t owned_by(const std::string& node_id) const {
        std::size_t count = 0;
        for (const auto& [part, owners] : owners_by_part) {
            if (owners.find(node_id) != std::string::npos) ++count;
        }
        return count;
    }
};

SqlPlacement placement_via_sql(Node& node) {
    QueryService service{node};
    const auto out = service.run(
        "SELECT partition_id, owner_nodes, owner_count, row_start, row_end "
        "FROM system.dataset_partitions;",
        "");
    EXPECT_TRUE(out.ok()) << out.error;
    SqlPlacement placement;
    for (const nlohmann::json& row : out.rows) {
        const std::string part = row.at("partition_id").get<std::string>();
        placement.owners_by_part[part] = row.at("owner_nodes").get<std::string>();
        placement.rf_by_part[part] = row.at("owner_count").get<int>();
        placement.ranges.emplace_back(row.at("row_start").get<std::uint64_t>(),
                                      row.at("row_end").get<std::uint64_t>());
    }
    std::sort(placement.ranges.begin(), placement.ranges.end());
    return placement;
}

// Assert a set of [start, end) ranges tiles [0, total) with no gap or overlap.
void expect_sql_ranges_tile(const SqlPlacement& placement, std::uint64_t total) {
    std::uint64_t expected_start = 0;
    for (const auto& [start, end] : placement.ranges) {
        EXPECT_EQ(start, expected_start) << "ranges must be contiguous";
        EXPECT_GT(end, start) << "a recorded partition holds at least one row";
        expected_start = end;
    }
    EXPECT_EQ(expected_start, total) << "ranges must cover the whole table";
}

}  // namespace

TEST(PartitionRebalance, CatalogConvergesOnEveryNodeAfterJoin) {
    // --- the writer (A): a partitioned table over 3 members ----------------------
    TempDir dir_a;
    Fixture fx{dir_a, /*count=*/60};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/64, /*replication_factor=*/1));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const auto write = zetta::write_partitioned(*fx.node, *fx.database, *fx.table,
                                                members);
    ASSERT_TRUE(write.ok()) << write.error;
    ASSERT_GT(write.partitions.size(), 3u);
    const std::uint64_t total_rows = write.rows_written;

    // --- a peer (B): converges to A's initial placement --------------------------
    TempDir dir_b;
    const std::string id_b = zetta::new_uuid();
    ReplicationManager repl_b{id_b};
    Node node_b{config_for(dir_b), &repl_b};
    node_b.start();

    auto gossip_a_to_b = [&] {
        repl_b.merge(fx.replication.save());
        node_b.tick();  // reconcile merged part:/lo: keys into B's catalog
    };
    gossip_a_to_b();

    const SqlPlacement a_initial = placement_via_sql(*fx.node);
    const SqlPlacement b_initial = placement_via_sql(node_b);
    ASSERT_FALSE(a_initial.owners_by_part.empty());
    EXPECT_EQ(a_initial.owners_by_part, b_initial.owners_by_part)
        << "initial placement must already match on every node";

    // --- a node joins: the production tick auto-trigger rebalances on A ----------
    // Membership is replicated `node:<id>` state. Seed the write-time members and
    // tick to baseline them (the first observation never rebalances, and the
    // partitions were placed over exactly these members → a no-op recompute).
    for (const auto& member : members) {
        fx.replication.set("node:" + member,
                           nlohmann::json{{"address", member + ":7777"}});
    }
    fx.node->tick();

    // node-d joins → membership delta → rebalance between jobs, captured into A's
    // CRDT and gossiped to B.
    fx.replication.set("node:node-d",
                       nlohmann::json{{"address", "node-d:7777"}});
    fx.node->tick();
    gossip_a_to_b();

    // --- the acceptance: identical new placement on every node -------------------
    const SqlPlacement a_after = placement_via_sql(*fx.node);
    const SqlPlacement b_after = placement_via_sql(node_b);

    // Same set of partitions, same owners, same replication factor — on both nodes.
    EXPECT_EQ(a_after.owners_by_part, b_after.owners_by_part)
        << "the rebalanced catalog must be identical on every node";
    EXPECT_EQ(a_after.rf_by_part, b_after.rf_by_part);
    EXPECT_EQ(a_after.owners_by_part.size(), a_initial.owners_by_part.size())
        << "no partition is lost or invented by a rebalance";

    // The new node took a share, but only ~1/N (the HRW win), on both nodes.
    const std::size_t a_owned_by_d = a_after.owned_by("node-d");
    EXPECT_GT(a_owned_by_d, 0u) << "the joiner took a share of the partitions";
    EXPECT_LT(a_owned_by_d, a_after.owners_by_part.size()) << "not a full rehash";
    EXPECT_EQ(b_after.owned_by("node-d"), a_owned_by_d);

    // Replication factor preserved (rf = 1), every owner a live member, on both.
    for (const auto& [part, count] : a_after.rf_by_part) {
        EXPECT_EQ(count, 1) << "rf preserved for " << part;
    }

    // Every row remains readable exactly once — the ranges still tile, on both.
    expect_sql_ranges_tile(a_after, total_rows);
    expect_sql_ranges_tile(b_after, total_rows);

    node_b.stop();
}

// ---- the "(or leaves)" clause: a departing node sheds its partitions ------------
//
// The auto-trigger fires on joins today (append-only node:<id> keys — the documented
// membership debt), but the rebalance library function handles any membership delta.
// When a node leaves, its partitions must re-place onto the survivors (no row is
// stranded on the departed node) while rf and the row tiling hold.

TEST(PartitionRebalance, LeaveReassignsPartitionsAwayFromDepartedNode) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/60};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/64, /*replication_factor=*/2));

    const std::vector<std::string> members{"node-a", "node-b", "node-c", "node-d"};
    const auto write = zetta::write_partitioned(*fx.node, *fx.database, *fx.table,
                                                members);
    ASSERT_TRUE(write.ok()) << write.error;

    // Before: node-d owns a share of the (rf=2) partitions.
    std::size_t owned_by_d_before = 0;
    for (const auto& partition : fx.node->dataset_partitions()) {
        if (as_set(partition->owner_nodes()).count("node-d")) ++owned_by_d_before;
    }
    ASSERT_GT(owned_by_d_before, 0u);

    // node-d leaves: rebalance against the survivors.
    const std::vector<std::string> survivors{"node-a", "node-b", "node-c"};
    const RebalanceResult result =
        zetta::rebalance_partitions(*fx.node, survivors, members);
    EXPECT_GT(result.moved, 0u);

    const std::set<std::string> survivor_set = as_set(survivors);
    for (const auto& partition : fx.node->dataset_partitions()) {
        const std::set<std::string> owners = as_set(partition->owner_nodes());
        // No partition references the departed node...
        EXPECT_FALSE(owners.count("node-d")) << "no row stranded on the departed node";
        // ...rf is preserved (still two distinct live owners)...
        EXPECT_EQ(partition->owner_nodes().size(), 2u);
        EXPECT_EQ(owners.size(), 2u) << "owners are distinct";
        for (const auto& owner : owners) {
            EXPECT_TRUE(survivor_set.count(owner)) << owner << " is a survivor";
        }
    }
    // ...and every row still tiles exactly once.
    expect_ranges_tile(*fx.node, write.rows_written);
}
