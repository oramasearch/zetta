#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/churn_dataset.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/storage/json_store.hpp"

// Shared harness for the testkit catalog (`zetta::testkit`) — the reusable pieces a
// case body builds on: an auto-cleaned scratch directory, a **real multi-node
// loopback cluster** (the same shape the distributed test suites stand up), and a
// self-contained synthetic churn dataset + its numeric encoding (so the ML /
// pipeline cases need no on-disk fixture and run from any binary). See
// docs/test-suite.md.
namespace zetta {

class Node;
class Cluster;

namespace testkit {

// An auto-cleaned scratch directory under the system temp dir — the data_dir for an
// ephemeral node. Created on construction, removed (best-effort) on destruction.
class ScratchDir {
public:
    ScratchDir();
    ~ScratchDir();
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// A real, fully-connected loopback cluster of N nodes on the local host — each a
// Node + a Cluster over TCP (127.0.0.1), brought up sequentially so every node
// dials the actual bound addresses of the earlier ones (robust to busy ports), then
// waited until membership converges. This is the genuine multi-node substrate the
// distributed cases run on (collectives, distributed ops) — not a simulation. Tears
// the cluster down on destruction.
class LoopbackCluster {
public:
    // Stand up `node_count` connected nodes. `connected()` reports whether
    // membership converged within the bring-up window.
    explicit LoopbackCluster(std::size_t node_count);
    ~LoopbackCluster();
    LoopbackCluster(const LoopbackCluster&) = delete;
    LoopbackCluster& operator=(const LoopbackCluster&) = delete;

    std::size_t size() const { return entries_.size(); }
    bool connected() const { return connected_; }

    Node& node(std::size_t index);
    Cluster& cluster(std::size_t index);
    const std::string& node_id(std::size_t index) const;

private:
    struct Entry;
    std::vector<std::unique_ptr<Entry>> entries_;
    bool connected_ = false;
};

// A self-contained synthetic churn dataset built in code (no on-disk fixture): a
// handful of accounts — including the FedEx Freight + sibling subsidiaries
// entity-resolution case (TC-4) — plus comms / usage events, with churn correlated
// to failed payments and a couple of deliberately post-`as_of` events (the leakage
// trap). Deterministic; the substrate the built-in ML / governance / pipeline cases
// use. A custom dataset directory (load_churn_dataset) can override it in a case.
ChurnDataset sample_churn_dataset();

// The churn dataset encoded into the two numeric DDF input shards the pipeline
// operates on (string ids / ISO dates / categorical event types -> the numeric
// columns the schemas name), plus the as-of cutoff as a day-of-year. Mirrors the
// encoding the GBDT / pipeline test suites use.
struct ChurnNumeric {
    std::vector<nlohmann::json> events;    // entity, ts, value, type
    std::vector<nlohmann::json> accounts;  // entity, mrr, seats, tenure_days, churned
    std::int64_t as_of = 0;
    std::map<std::string, std::int64_t> entity_index;  // account_id -> entity key
};
ChurnNumeric encode_churn(const ChurnDataset& dataset);

// The schemas + specs the encoded shards feed (the churn featurization graph + GBDT
// knobs the pipeline uses). Exposed so the ML and pipeline cases share one source.
ddf::DataFrameSchema churn_events_schema();
ddf::DataFrameSchema churn_accounts_schema();
ddf::FeaturizeSpec churn_featurize_spec(std::int64_t as_of);
ml::GbdtParams churn_gbdt_params(int num_trees, int max_depth, int max_bins,
                                 double learning_rate);

// Build an in-memory JsonStore from a set of numeric rows (the connect-step input).
JsonStore make_store(const std::vector<nlohmann::json>& rows);

}  // namespace testkit
}  // namespace zetta
