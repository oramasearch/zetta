#include "zetta/testkit/harness.hpp"

#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::testkit {
namespace {

// Process-wide port allocator: each loopback cluster gets a fresh window so a
// previous run's sockets (lingering in TIME_WAIT) never collide with this one. Each
// node binds within an 8-port window inside its cluster's stride.
std::atomic<std::uint16_t> g_next_base{30000};
constexpr std::uint16_t kClusterStride = 256;  // ports reserved per cluster
constexpr std::uint16_t kNodeWindow = 8;       // bind range per node

std::uint16_t reserve_cluster_base() {
    std::uint16_t base = g_next_base.fetch_add(kClusterStride);
    // Wrap well below the ephemeral range; the stride keeps node windows disjoint.
    if (base > static_cast<std::uint16_t>(60000)) {
        g_next_base.store(30000);
        base = g_next_base.fetch_add(kClusterStride);
    }
    return base;
}

}  // namespace

// ----- ScratchDir ------------------------------------------------------------

ScratchDir::ScratchDir() {
    std::error_code ec;
    path_ = std::filesystem::temp_directory_path(ec) / "zetta-testkit" / zetta::util::new_uuid();
    std::filesystem::create_directories(path_, ec);
}

ScratchDir::~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

// ----- LoopbackCluster -------------------------------------------------------

struct LoopbackCluster::Entry {
    ScratchDir dir;
    std::string id;  // the replication actor id doubles as the cluster node id
    std::unique_ptr<zetta::cluster::ReplicationManager> replication;  // replicated mode only
    std::unique_ptr<Node> node;
    std::unique_ptr<zetta::data::ReplicatedBlobStore> blobs;  // replicated mode only
    std::unique_ptr<zetta::cluster::Cluster> cluster;
};

LoopbackCluster::LoopbackCluster(std::size_t node_count, const LoopbackOptions& options) {
    const std::uint16_t base = reserve_cluster_base();
    std::vector<std::string> seeds;  // actual bound addresses of earlier nodes

    for (std::size_t index = 0; index < node_count; ++index) {
        auto entry = std::make_unique<Entry>();
        entry->id = zetta::util::new_uuid();

        NodeConfig node_config;
        node_config.data_dir = entry->dir.path().string();
        node_config.preflight = false;  // fresh scratch dir; nothing to check
        if (options.replicated || options.attached) {
            // The live-process wiring: the CRDT document behind the Node, the
            // blob store behind the cluster link (bodies transfer peer-to-peer).
            entry->replication =
                std::make_unique<zetta::cluster::ReplicationManager>(entry->id);
            entry->node = std::make_unique<Node>(node_config, entry->replication.get());
            entry->blobs = std::make_unique<zetta::data::ReplicatedBlobStore>(
                entry->dir.path(), *entry->replication);
        } else {
            entry->node = std::make_unique<Node>(node_config);
        }
        entry->node->start();

        zetta::cluster::ClusterConfig cluster_config;
        cluster_config.listen_host = "127.0.0.1";
        cluster_config.listen_port =
            static_cast<std::uint16_t>(base + index * kNodeWindow);
        cluster_config.port_range = kNodeWindow;
        cluster_config.seeds = seeds;
        entry->cluster = std::make_unique<zetta::cluster::Cluster>(
            entry->id, std::move(cluster_config), entry->replication.get(),
            entry->blobs.get());
        entry->cluster->start();

        // Dial the actually-bound address (port_range may have stepped over a busy
        // port), so the next node's seed list is always correct.
        seeds.push_back(entry->cluster->listen_address());

        if (options.attached) {
            // The rest of the live-process wiring, in main()'s order: announce
            // this node as a replicated member, then hand the cluster to the
            // node so its compute plane (and the run.launch listener behind
            // every co-launched job kind) comes up against it.
            entry->replication->set(
                "node:" + entry->id,
                nlohmann::json{{"address", entry->cluster->listen_address()}});
            entry->node->attach_cluster(*entry->cluster, options.worker_count);
        }
        entries_.push_back(std::move(entry));
    }

    // Wait for membership to converge (every node sees the other N-1).
    const std::size_t expected = entries_.empty() ? 0 : entries_.size() - 1;
    connected_ = true;
    for (int attempt = 0; attempt < 240 && expected > 0; ++attempt) {
        bool all = true;
        for (const auto& entry : entries_) {
            if (entry->cluster->peer_count() != expected) {
                all = false;
                break;
            }
        }
        if (all) break;
        connected_ = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (expected > 0) {
        connected_ = true;
        for (const auto& entry : entries_) {
            if (entry->cluster->peer_count() != expected) connected_ = false;
        }
    }
}

LoopbackCluster::~LoopbackCluster() {
    // Tear down networking before the nodes (reverse of bring-up).
    for (auto& entry : entries_) {
        if (entry->cluster) entry->cluster->stop();
    }
    for (auto& entry : entries_) {
        if (entry->node) entry->node->stop();
    }
}

Node& LoopbackCluster::node(std::size_t index) { return *entries_.at(index)->node; }
zetta::cluster::Cluster& LoopbackCluster::cluster(std::size_t index) {
    return *entries_.at(index)->cluster;
}
const std::string& LoopbackCluster::node_id(std::size_t index) const {
    return entries_.at(index)->cluster->node_id();
}

void LoopbackCluster::announce_member(std::size_t index) {
    Entry& entry = *entries_.at(index);
    if (!entry.replication) return;  // membership keys are replicated state
    entry.replication->set(
        "node:" + entry.id,
        nlohmann::json{{"address", entry.cluster->listen_address()}});
}

nlohmann::json LoopbackCluster::replicated_document(std::size_t index) const {
    const Entry& entry = *entries_.at(index);
    if (!entry.replication) return nlohmann::json::object();
    return entry.replication->snapshot();
}

// ----- synthetic churn dataset (in code, no on-disk fixture) -----------------

namespace {

zetta::usecases::churn::ChurnAccount account(std::string id, std::string name, std::string brand,
                     std::string entity, std::string tier, double mrr,
                     std::int64_t seats, std::int64_t tenure, bool churned) {
    zetta::usecases::churn::ChurnAccount a;
    a.account_id = std::move(id);
    a.name = std::move(name);
    a.brand = std::move(brand);
    a.entity = std::move(entity);
    a.plan_tier = std::move(tier);
    a.industry = "logistics";
    a.region = "us";
    a.mrr = mrr;
    a.seats = seats;
    a.tenure_days = tenure;
    a.renewal_date = "2026-05-15";
    a.churned = churned;
    a.churn_date = churned ? "2026-04-10" : "";
    return a;
}

zetta::usecases::churn::ChurnEvent event(std::string id, std::string account_id, std::string ts,
                 std::string type, double value, std::string mention = {}) {
    zetta::usecases::churn::ChurnEvent e;
    e.event_id = std::move(id);
    e.account_id = std::move(account_id);
    e.event_ts = std::move(ts);
    e.event_type = std::move(type);
    e.channel = mention.empty() ? "app" : "crm";
    e.mention = std::move(mention);
    e.value = value;
    return e;
}

}  // namespace

zetta::usecases::churn::ChurnDataset sample_churn_dataset() {
    zetta::usecases::churn::ChurnDataset dataset;
    dataset.name = "testkit-synthetic-churn";
    dataset.description = "in-code synthetic churn dataset for the test suite";
    dataset.entity_field = "account_id";
    dataset.time_field = "event_ts";
    dataset.label_field = "churned";
    dataset.as_of_date = "2026-03-01";  // t0
    dataset.churn_horizon_days = 90;
    dataset.label_definition = "churned within the horizon after t0";

    // The FedEx entity-resolution case (TC-4): six sibling subsidiaries, each its
    // own account. Office churned (it has the most payment failures).
    dataset.accounts = {
        account("acct_fedex_freight", "FedEx Freight", "FedEx", "fedex_freight",
                "enterprise", 14000, 320, 1180, false),
        account("acct_fedex_ground", "FedEx Ground", "FedEx", "fedex_ground",
                "enterprise", 16000, 410, 1500, false),
        account("acct_fedex_express", "FedEx Express", "FedEx", "fedex_express",
                "enterprise", 22000, 600, 2100, false),
        account("acct_fedex_office", "FedEx Office", "FedEx", "fedex_office",
                "business", 4200, 60, 540, true),
        account("acct_fedex_logistics", "FedEx Logistics", "FedEx",
                "fedex_logistics", "enterprise", 9000, 150, 800, false),
        account("acct_fedex_services", "FedEx Services", "FedEx", "fedex_services",
                "business", 3000, 40, 300, false),
        // A few standalone accounts; two churned with payment failures.
        account("acct_acme", "Acme Corp", "", "acme", "business", 5200, 80, 640, true),
        account("acct_globex", "Globex", "", "globex", "enterprise", 12000, 240, 1320, false),
        account("acct_initech", "Initech", "", "initech", "starter", 1500, 18, 210, true),
        account("acct_umbrella", "Umbrella Logistics", "", "umbrella", "business",
                4800, 70, 560, false),
    };

    // Events. Comms rows carry a mention (the resolution case). Churned accounts
    // accumulate payment failures (type payment_failed) before t0 — the signal the
    // model should pick up. A couple of events sit AFTER t0 (the leakage trap a
    // point-in-time featurizer must drop).
    int next = 1;
    auto id = [&next] { return "evt_" + std::to_string(next++); };
    std::vector<zetta::usecases::churn::ChurnEvent> events;

    // Comms mentions for every FedEx sibling (drives the resolution case), incl. a
    // spelling variant the FullName resolver still matches.
    events.push_back(event(id(), "acct_fedex_freight", "2026-02-12T15:04:00Z", "comms", 0, "FedEx Freight"));
    events.push_back(event(id(), "acct_fedex_freight", "2026-01-28T09:30:00Z", "comms", 0, "Fedex Freight"));
    events.push_back(event(id(), "acct_fedex_ground", "2026-02-20T11:00:00Z", "comms", 0, "FedEx Ground"));
    events.push_back(event(id(), "acct_fedex_express", "2026-02-05T16:45:00Z", "comms", 0, "FedEx Express"));
    events.push_back(event(id(), "acct_fedex_office", "2026-01-15T10:10:00Z", "comms", 0, "FedEx Office"));
    events.push_back(event(id(), "acct_fedex_logistics", "2026-02-18T13:20:00Z", "comms", 0, "FedEx Logistics"));
    events.push_back(event(id(), "acct_fedex_services", "2026-02-22T08:05:00Z", "comms", 0, "FedEx Services"));

    // Payment failures: heavy on churned accounts, light/none on healthy ones.
    auto payment_failures = [&](const std::string& acct, int count) {
        for (int i = 0; i < count; ++i) {
            const std::string day = (i % 2 == 0) ? "10" : "20";
            const std::string month = (i < 2) ? "01" : "02";
            events.push_back(event(id(), acct, "2026-" + month + "-" + day + "T12:00:00Z",
                                   "payment_failed", 1));
        }
    };
    payment_failures("acct_fedex_office", 4);  // churned
    payment_failures("acct_acme", 3);          // churned
    payment_failures("acct_initech", 3);       // churned
    payment_failures("acct_fedex_freight", 1);
    payment_failures("acct_globex", 1);

    // Support tickets with a severity (the support_severity feature).
    events.push_back(event(id(), "acct_fedex_office", "2026-02-01T09:00:00Z", "support_ticket", 3));
    events.push_back(event(id(), "acct_acme", "2026-02-09T09:00:00Z", "support_ticket", 2));
    events.push_back(event(id(), "acct_globex", "2026-02-14T09:00:00Z", "support_ticket", 1));

    // Engagement (logins) so healthy accounts have activity.
    for (const zetta::usecases::churn::ChurnAccount& a : dataset.accounts) {
        events.push_back(event(id(), a.account_id, "2026-02-25T08:00:00Z", "login", 1));
        events.push_back(event(id(), a.account_id, "2026-02-26T08:00:00Z", "feature_use", 1));
    }

    // The leakage trap: events AFTER t0 (2026-03-01) — must never enter a feature.
    events.push_back(event(id(), "acct_fedex_office", "2026-03-15T12:00:00Z", "payment_failed", 1));
    events.push_back(event(id(), "acct_globex", "2026-03-20T09:00:00Z", "support_ticket", 2));

    dataset.events = std::move(events);
    return dataset;
}

// ----- testkit-specific encoding extras --------------------------------------
// The churn numeric encoding + the standard pipeline schemas / featurize spec are
// the canonical ones in churn_solution.cpp (zetta::usecases::churn::encode_churn_dataset /
// churn_events_schema / churn_accounts_schema / churn_featurize_spec), reused here
// rather than copied. Only the tunable GBDT params live in the testkit.

ml::GbdtParams churn_gbdt_params(int num_trees, int max_depth, int max_bins,
                                 double learning_rate) {
    ml::GbdtParams params;
    params.num_trees = num_trees;
    params.max_depth = max_depth;
    params.max_bins = max_bins;
    params.learning_rate = learning_rate;
    params.min_child_hessian = 1e-3;
    return params;
}

}  // namespace zetta::testkit
