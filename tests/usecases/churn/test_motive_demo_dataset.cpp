#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/solution_catalog.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"

// **The real GBDT + TreeSHAP over the Motive demo dataset.**
//
// Every other measurement of that dataset came from an offline linear probe, which says
// whether a signal EXISTS but not what the engine actually learns or what it says drives
// churn. The product's answer to *"why is this account at risk?"* is TreeSHAP over a
// trained gradient-boosted model, and until this ran, that answer had never been produced
// on Motive-shaped data at all — only on the small synthetic rows the other tests build.
//
// **Gated on the dataset being present**, following the Qwen-checkpoint precedent: the
// rows are generated (`scripts/gen_motive_demo_dataset.py`), not committed, so a normal
// checkout skips this rather than failing. Run it with:
//
//     python3 scripts/gen_motive_demo_dataset.py --out data/demo/motive --accounts 1500
//     ZETTA_MOTIVE_DEMO_DIR=data/demo/motive ./build/tests/zetta-tests \
//         --gtest_filter='MotiveDemoDataset.*'
//
// The numbers it prints are the ones the client-facing report quotes
// (docs/reports/results/motive-churn-results-2026-08-04.md). Re-run it when the generator
// changes — a report whose numbers no longer reproduce is worse than no report.
namespace {

namespace fs = std::filesystem;

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::CpuDevice;
using zetta::pipeline::Solution;
using zetta::pipeline::SolutionCatalog;
using zetta::usecases::churn::ChurnSolutionOptions;
using zetta::usecases::churn::ChurnSolutionResult;
using zetta::usecases::churn::ChurnTableInput;
using Json = nlohmann::json;

struct AttachedNode {
    explicit AttachedNode(std::uint16_t port)
        : node(node_config(temp_dir)), cluster("motive-demo", cluster_config(port)) {
        node.start();
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);
    }
    ~AttachedNode() { node.stop(); }
    static NodeConfig node_config(const zetta::test::TempDir& dir) {
        NodeConfig cfg;
        cfg.data_dir = dir.path().string();
        return cfg;
    }
    static ClusterConfig cluster_config(std::uint16_t port) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        return config;
    }
    zetta::test::TempDir temp_dir;
    Node node;
    Cluster cluster;
};

// The dataset directory, or empty when it has not been generated.
fs::path demo_dir() {
    if (const char* from_env = std::getenv("ZETTA_MOTIVE_DEMO_DIR")) return fs::path(from_env);
    const fs::path fallback = fs::path(ZETTA_SOURCE_DIR) / "data" / "demo" / "motive";
    return fs::exists(fallback / "account_features.csv") ? fallback : fs::path{};
}

// Trim trailing CR — Python's csv writer emits CRLF, so without this the LAST column of
// every row is named "churned\r", which matches nothing and prints identically to
// "churned" in any error message. The engine's own CSV reader handles it; this hand-rolled
// one is only here to avoid depending on that path while testing it.
std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                             text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

// A minimal CSV reader: the generator writes plain unquoted values, so a split on commas
// is enough and pulling in a parser would be more machinery than the job needs.
std::vector<Json> read_csv(const fs::path& path) {
    std::vector<Json> rows;
    std::ifstream file(path);
    if (!file) return rows;
    std::string line;
    if (!std::getline(file, line)) return rows;
    std::vector<std::string> header;
    for (std::stringstream stream(line); std::getline(stream, line, ',');) {
        header.push_back(trim(line));
    }
    while (std::getline(file, line)) {
        std::vector<std::string> cells;
        for (std::stringstream stream(line); std::getline(stream, line, ',');) {
            cells.push_back(trim(line));
        }
        if (cells.size() != header.size()) continue;
        Json row = Json::object();
        for (std::size_t index = 0; index < header.size(); ++index) {
            const std::string& cell = cells[index];
            // Type it the way the ingest path would: a number stays a number.
            char* end = nullptr;
            const double value = std::strtod(cell.c_str(), &end);
            if (!cell.empty() && end != nullptr && *end == '\0') {
                if (value == static_cast<double>(static_cast<std::int64_t>(value))) {
                    row[header[index]] = static_cast<std::int64_t>(value);
                } else {
                    row[header[index]] = value;
                }
            } else {
                row[header[index]] = cell;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace

// =====================================================================
// Train the real model on the demo dataset and read what drives churn.
// =====================================================================

TEST(MotiveDemoDataset, TrainsAndExplainsWithTreeShap) {
    const fs::path dir = demo_dir();
    if (dir.empty()) {
        GTEST_SKIP() << "generate it first: python3 scripts/gen_motive_demo_dataset.py "
                        "--out data/demo/motive --accounts 1500";
    }
    std::vector<Json> rows = read_csv(dir / "account_features.csv");
    ASSERT_FALSE(rows.empty()) << "could not read " << (dir / "account_features.csv");

    const std::size_t accounts = rows.size();
    std::size_t churned = 0;
    for (const Json& row : rows) churned += row.value("churned", 0) == 1;

    AttachedNode host(24730);
    zetta::data::AddDatasetOptions options;
    options.database = "motive_demo";
    options.table_name = "account_features";
    options.partitioned = true;
    options.members = {host.node.node_id()};
    const zetta::data::AddDatasetResult added =
        zetta::data::add_dataset_rows(host.node, std::move(rows), options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_TRUE(added.table);

    // The contract: the entity key is an integer ordinal, since sfdc_account_id is a
    // string — the documented limitation, and the reason the override exists.
    zetta::data::DatasetContract overrides;
    overrides.entity_column = "account_index";
    overrides.label_column = "churned";
    const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("motive_demo.account_features"), overrides,
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;

    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    CpuDevice device;
    ChurnSolutionOptions run_options;
    run_options.project = "motive-demo";
    run_options.model_name = "motive_churn_model";
    run_options.coordinate = false;
    ChurnTableInput input;
    input.table_id = resolved.table_id;
    input.contract = resolved.contract;

    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_table(
        host.node, device, solution->graph, input, run_options);
    ASSERT_TRUE(result.ok()) << result.execution.error;

    const std::shared_ptr<zetta::ml::Model> model = host.node.find_model(result.model_id);
    ASSERT_TRUE(model);
    const Json metrics = model->value().metrics;

    // ---- report the numbers ------------------------------------------------
    std::cout << "\n=== Motive demo dataset · real GBDT + TreeSHAP ===\n";
    std::cout << "accounts        " << accounts << "  (churned " << churned << ", "
              << (100.0 * static_cast<double>(churned) / static_cast<double>(accounts))
              << "%)\n";
    std::cout << "features        " << resolved.contract.feature_columns.size() << "\n";
    std::cout << "metrics         " << metrics.dump() << "\n";
    std::cout << "top driver      " << result.top_driver << "\n";
    std::cout << "driver ranking (TreeSHAP, mean |phi|, strongest first):\n";
    for (const auto& [feature, weight] : result.drivers) {
        std::cout << "  " << feature << "  " << weight << "\n";
    }
    std::cout << "at-risk list    " << result.ranked.size() << " accounts, top 5:\n";
    for (std::size_t index = 0; index < std::min<std::size_t>(5, result.ranked.size());
         ++index) {
        std::cout << "  entity " << result.ranked[index].entity << "  risk "
                  << result.ranked[index].risk << "\n";
    }
    std::cout << std::endl;

    // ---- the assertions that make it a test, not a print --------------------
    EXPECT_EQ(result.ranked.size(), accounts);
    ASSERT_FALSE(result.drivers.empty()) << "TreeSHAP produced no drivers";
    EXPECT_EQ(result.drivers.size(), resolved.contract.feature_columns.size());
    EXPECT_FALSE(result.top_driver.empty());

    // The model must learn a real signal — and NOT a perfect one. The label is drawn from
    // a logistic on the latent, so the achievable ceiling is bounded; an AUC near 1.0
    // would mean the generator leaked the answer into a column, which is a defect in the
    // dataset rather than a good model.
    ASSERT_TRUE(metrics.contains("auc")) << metrics.dump();
    const double auc = metrics.at("auc").get<double>();
    EXPECT_GT(auc, 0.70) << "the model did not learn the planted signal";
    EXPECT_LT(auc, 0.95) << "suspiciously separable — a column is giving the label away";

    // The dominant driver must be one of the four channels the label was actually built
    // from, never a size control. If `seats` or `tenure_days` came top, the model would be
    // ranking accounts by how big they are.
    const std::vector<std::string> real_drivers = {
        "escalations_90d", "billing_disputes_180d", "days_since_last_sales_touch",
        "unresolved_contacts_90d", "escalation_trend", "sales_touch_trend",
        "chat_deflection_failures_90d", "nps_score", "nps_is_detractor"};
    EXPECT_NE(std::find(real_drivers.begin(), real_drivers.end(), result.top_driver),
              real_drivers.end())
        << "dominant driver was '" << result.top_driver << "' — a size control, not a signal";

    // Risks are probabilities, ranked descending.
    for (std::size_t index = 1; index < result.ranked.size(); ++index) {
        ASSERT_GE(result.ranked[index - 1].risk, result.ranked[index].risk);
    }
    for (const auto& score : result.ranked) {
        ASSERT_GE(score.risk, 0.0);
        ASSERT_LE(score.risk, 1.0);
    }
}
