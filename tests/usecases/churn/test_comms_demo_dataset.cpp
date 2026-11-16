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
#include "zetta/ml/score_table.hpp"
#include <chrono>
#include "zetta/node.hpp"
#include "zetta/pipeline/solution_catalog.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"

// **The real GBDT + TreeSHAP over the communications demo dataset.**
//
// Every other measurement of that dataset came from an offline linear probe, which says
// whether a signal EXISTS but not what the engine actually learns or what it says drives
// churn. The product's answer to *"why is this account at risk?"* is TreeSHAP over a
// trained gradient-boosted model, and until this ran, that answer had never been produced
// on communications-shaped data at all — only on the small synthetic rows the other tests build.
//
// **Gated on the dataset being present**, following the Qwen-checkpoint precedent: the
// rows are generated (`scripts/gen_comms_demo_dataset.py`), not committed, so a normal
// checkout skips this rather than failing. Run it with:
//
//     python3 scripts/gen_comms_demo_dataset.py --out data/demo/comms --accounts 1500
//     ZETTA_COMMS_DEMO_DIR=data/demo/comms ./build/tests/zetta-tests \
//         --gtest_filter='CommsDemoDataset.*'
//
// The numbers it prints are the ones a run report quotes
// (docs/runbooks/regenerating-the-run-report.md). Re-run it when the generator changes —
// a report whose numbers no longer reproduce is worse than no report.
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
        : node(node_config(temp_dir)), cluster("comms-demo", cluster_config(port)) {
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
    if (const char* from_env = std::getenv("ZETTA_COMMS_DEMO_DIR")) return fs::path(from_env);
    const fs::path fallback = fs::path(ZETTA_SOURCE_DIR) / "data" / "demo" / "comms";
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

TEST(CommsDemoDataset, TrainsAndExplainsWithTreeShap) {
    const fs::path dir = demo_dir();
    if (dir.empty()) {
        GTEST_SKIP() << "generate it first: python3 scripts/gen_comms_demo_dataset.py "
                        "--out data/demo/comms --accounts 1500";
    }
    std::vector<Json> rows = read_csv(dir / "account_features.csv");
    ASSERT_FALSE(rows.empty()) << "could not read " << (dir / "account_features.csv");

    const std::size_t accounts_total = rows.size();

    // A GENUINE holdout. The engine's reported AUC is computed over the rows each node
    // TRAINED on — every node scores its own local rows and the pairs are gathered — so it
    // is an in-sample figure and must never be published as though it were held out. To
    // get the number that predicts behaviour on unseen accounts, train on 70% and score
    // the other 30% through the model afterwards.
    //
    // The split is every third account, not a random sample: deterministic, so the figure
    // is reproducible, and stratified enough at a 17.5% base rate that both sides carry
    // churners.
    std::vector<Json> train_rows;
    std::vector<Json> holdout_rows;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        (index % 10 < 7 ? train_rows : holdout_rows).push_back(rows[index]);
    }
    ASSERT_FALSE(holdout_rows.empty());

    std::size_t churned = 0;
    for (const Json& row : rows) churned += row.value("churned", 0) == 1;
    rows = train_rows;
    const std::size_t accounts = rows.size();

    AttachedNode host(24730);
    zetta::data::AddDatasetOptions options;
    options.database = "comms_demo";
    options.table_name = "account_features";
    options.partitioned = true;
    options.members = {host.node.node_id()};
    const zetta::data::AddDatasetResult added =
        zetta::data::add_dataset_rows(host.node, std::move(rows), options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_TRUE(added.table);

    // The contract: the entity key is an integer ordinal, since crm_account_id is a
    // string — the documented limitation, and the reason the override exists.
    zetta::data::DatasetContract overrides;
    overrides.entity_column = "account_index";
    overrides.label_column = "churned";
    const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
        host.node, zetta::data::parse_dataset_path("comms_demo.account_features"), overrides,
        zetta::data::default_dataset_sampler(host.node, &host.cluster));
    ASSERT_TRUE(resolved.ok()) << resolved.error;

    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    CpuDevice device;
    ChurnSolutionOptions run_options;
    run_options.project = "comms-demo";
    run_options.model_name = "comms_churn_model";
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
    // ---- the holdout figure -------------------------------------------------
    // Score the 30% the model never saw, and compute AUC over it here. This is the
    // number that says how the model behaves on an account it has not met.
    // The model's inputs by original column name: the numeric features, and each
    // text category as its text value (the model one-hot encodes it, #1203).
    std::vector<std::string> model_inputs = resolved.contract.feature_columns;
    std::size_t encoded_features = resolved.contract.feature_columns.size();
    for (const zetta::data::CategoricalEncoding& category :
         resolved.contract.categorical_features) {
        model_inputs.push_back(category.column);
        encoded_features += category.feature_names().size();
    }
    std::vector<std::int64_t> holdout_entities;
    std::vector<double> holdout_labels;
    std::vector<Json> holdout_features;
    for (const Json& row : holdout_rows) {
        Json features = Json::object();
        for (const std::string& column : model_inputs) {
            features[column] = row.at(column);
        }
        holdout_features.push_back(std::move(features));
        holdout_entities.push_back(row.at("account_index").get<std::int64_t>());
        holdout_labels.push_back(static_cast<double>(row.value("churned", 0)));
    }
    const zetta::ml::TableScoreResult holdout =
        zetta::ml::score_rows(*model, holdout_features, holdout_entities);
    ASSERT_TRUE(holdout.ok()) << holdout.error;
    ASSERT_EQ(holdout.ranked.size(), holdout_rows.size());

    // Mann-Whitney AUC over the held-out rows.
    std::map<std::int64_t, double> risk_of;
    for (const auto& score : holdout.ranked) risk_of[score.entity] = score.risk;
    std::vector<double> positives;
    std::vector<double> negatives;
    for (std::size_t index = 0; index < holdout_entities.size(); ++index) {
        (holdout_labels[index] > 0.5 ? positives : negatives)
            .push_back(risk_of[holdout_entities[index]]);
    }
    ASSERT_FALSE(positives.empty());
    ASSERT_FALSE(negatives.empty());
    std::sort(negatives.begin(), negatives.end());
    double concordant = 0.0;
    for (const double positive : positives) {
        const auto lower = std::lower_bound(negatives.begin(), negatives.end(), positive);
        const auto upper = std::upper_bound(negatives.begin(), negatives.end(), positive);
        concordant += static_cast<double>(lower - negatives.begin()) +
                      0.5 * static_cast<double>(upper - lower);
    }
    const double holdout_auc =
        concordant / (static_cast<double>(positives.size()) *
                      static_cast<double>(negatives.size()));

    // Sanity check on the AUC arithmetic above: score the TRAINING rows the same way
    // and confirm the figure matches what the engine reported in-sample. If these two
    // disagree, the holdout number is measuring the wrong thing.
    std::vector<Json> train_features;
    std::vector<std::int64_t> train_entities;
    std::vector<double> train_labels;
    for (const Json& row : train_rows) {
        Json features = Json::object();
        for (const std::string& column : model_inputs) {
            features[column] = row.at(column);
        }
        train_features.push_back(std::move(features));
        train_entities.push_back(row.at("account_index").get<std::int64_t>());
        train_labels.push_back(static_cast<double>(row.value("churned", 0)));
    }
    const zetta::ml::TableScoreResult in_sample =
        zetta::ml::score_rows(*model, train_features, train_entities);
    ASSERT_TRUE(in_sample.ok()) << in_sample.error;
    std::map<std::int64_t, double> train_risk;
    for (const auto& score : in_sample.ranked) train_risk[score.entity] = score.risk;
    std::vector<double> train_pos;
    std::vector<double> train_neg;
    for (std::size_t index = 0; index < train_entities.size(); ++index) {
        (train_labels[index] > 0.5 ? train_pos : train_neg)
            .push_back(train_risk[train_entities[index]]);
    }
    std::sort(train_neg.begin(), train_neg.end());
    double train_conc = 0.0;
    for (const double positive : train_pos) {
        const auto lower = std::lower_bound(train_neg.begin(), train_neg.end(), positive);
        const auto upper = std::upper_bound(train_neg.begin(), train_neg.end(), positive);
        train_conc += static_cast<double>(lower - train_neg.begin()) +
                      0.5 * static_cast<double>(upper - lower);
    }
    const double recomputed_in_sample =
        train_conc / (static_cast<double>(train_pos.size()) *
                      static_cast<double>(train_neg.size()));

    std::cout << "\n=== Communications demo dataset · real GBDT + TreeSHAP ===\n";
    std::cout << "in-sample AUC recomputed here " << recomputed_in_sample
              << " (engine said " << metrics.at("auc").get<double>()
              << ") — these must agree\n";
    std::cout << "accounts        " << accounts_total << " total ("
              << accounts << " train / " << holdout_rows.size() << " holdout), churned "
              << churned << " ("
              << (100.0 * static_cast<double>(churned) /
                  static_cast<double>(accounts_total))
              << "%)\n";
    std::cout << "HOLDOUT AUC     " << holdout_auc
              << "   <-- the number to publish (rows the model never saw)\n";
    std::cout << "features        " << resolved.contract.feature_columns.size()
              << " numeric, " << resolved.contract.categorical_features.size()
              << " categorical (" << encoded_features << " encoded)\n";
    std::cout << "in-sample       " << metrics.dump()
              << "   <-- computed over the TRAINING rows, not a holdout\n";
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

    // ---- the run's facts, as an artifact -------------------------------------
    // Everything a report needs, and NOTHING a narrator would have to compute. The
    // separation is the point: the engine produces every number here, and the narrative
    // layer may only arrange them into prose. A narrator that can compute is a narrator
    // that can be wrong about a figure.
    std::map<std::int64_t, Json> account_of;
    for (const Json& row : holdout_rows) account_of[row.at("account_index").get<std::int64_t>()] = row;
    for (const Json& row : train_rows) account_of[row.at("account_index").get<std::int64_t>()] = row;

    // The event count is the generator's, from its manifest: the test reads only the
    // rollup, so it cannot count the events itself.
    std::int64_t events_total = 0;
    if (std::ifstream manifest_file(dir / "manifest.json"); manifest_file) {
        const Json manifest = Json::parse(manifest_file, nullptr, false);
        if (!manifest.is_discarded() && manifest.contains("facts")) {
            events_total = manifest["facts"].value("events", std::int64_t{0});
        }
    }

    Json artifact;
    artifact["dataset"] = {{"accounts", accounts_total},
                           {"events", events_total},
                           {"churn_rate", static_cast<double>(churned) /
                                              static_cast<double>(accounts_total)},
                           {"features", model_inputs}};
    artifact["model"] = {{"id", result.model_id},
                         {"held_out_auc", holdout_auc},
                         {"in_sample_auc", metrics.at("auc").get<double>()},
                         {"accuracy", metrics.at("accuracy").get<double>()},
                         {"log_loss", metrics.at("log_loss").get<double>()},
                         {"train_rows", accounts},
                         {"holdout_rows", holdout_rows.size()}};
    Json drivers = Json::array();
    for (const auto& [feature, weight] : result.drivers) {
        drivers.push_back({{"feature", feature}, {"weight", weight}});
    }
    artifact["drivers"] = std::move(drivers);

    Json at_risk = Json::array();
    for (std::size_t index = 0; index < std::min<std::size_t>(20, result.ranked.size());
         ++index) {
        const std::int64_t entity = result.ranked[index].entity;
        Json entry;
        entry["rank"] = index + 1;
        entry["entity"] = entity;
        entry["risk"] = result.ranked[index].risk;
        if (const auto found = account_of.find(entity); found != account_of.end()) {
            // The three feature values a reader would want beside a risk score. Chosen by
            // the driver ranking, not hand-picked, so the report cannot quietly showcase
            // whichever numbers happen to look worst.
            for (std::size_t d = 0; d < std::min<std::size_t>(3, result.drivers.size()); ++d) {
                const std::string& feature = result.drivers[d].first;
                if (found->second.contains(feature)) {
                    entry["evidence"][feature] = found->second.at(feature);
                }
            }
        }
        at_risk.push_back(std::move(entry));
    }
    artifact["at_risk"] = std::move(at_risk);

    // ---- where does scoring a whole table spend its time? (#956) ------------
    // The run trains in seconds and a `score` over the same table did not return
    // in ten minutes through the API. Two candidates: resolving the dataset,
    // which samples via SELECT * because the SQL subset has no LIMIT, or the
    // scoring itself. Timing them apart is the whole diagnosis.
    //
    // Measured over the WHOLE generated table (every account, not the 70% the
    // model trained on), landed beside the training table, so the figure is the
    // one a `score` over the demo table pays. Each half runs five times and the
    // median is printed: one cold run says more about the allocator than the code.
    {
        std::vector<Json> all_rows = read_csv(dir / "account_features.csv");
        zetta::data::AddDatasetOptions all_options;
        all_options.database = "comms_demo";
        all_options.table_name = "account_features_all";
        all_options.partitioned = true;
        all_options.members = {host.node.node_id()};
        const zetta::data::AddDatasetResult all_added =
            zetta::data::add_dataset_rows(host.node, std::move(all_rows), all_options);
        ASSERT_TRUE(all_added.ok()) << all_added.error;

        using Clock = std::chrono::steady_clock;
        const auto micros = [](Clock::time_point from, Clock::time_point to) {
            return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
        };
        const auto median = [](std::vector<long long> samples) {
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2];
        };

        // Resolved twice over: with the entity and label named (what #956's
        // `score` sent) and with only the entity named (the label inferred, which
        // must check that `churned` is binary). The entity is named in both: the
        // table's key, `account_index`, is not a name inference recognizes.
        constexpr int kRepeats = 5;
        std::vector<long long> resolve_named;
        std::vector<long long> resolve_inferred;
        zetta::data::DatasetContract entity_only;
        entity_only.entity_column = "account_index";
        std::vector<long long> score_times;
        zetta::data::ResolvedDataset again;
        zetta::data::ResolvedDataset inferred;
        std::uint64_t rows_scored = 0;
        for (int repeat = 0; repeat < kRepeats; ++repeat) {
            const auto named_start = Clock::now();
            again = zetta::data::resolve_dataset(
                host.node, zetta::data::parse_dataset_path("comms_demo.account_features_all"),
                overrides, {}, zetta::data::kDefaultSampleRows, &host.cluster);
            resolve_named.push_back(micros(named_start, Clock::now()));
            ASSERT_TRUE(again.ok()) << again.error;

            const auto inferred_start = Clock::now();
            inferred = zetta::data::resolve_dataset(
                host.node, zetta::data::parse_dataset_path("comms_demo.account_features_all"),
                entity_only, {}, zetta::data::kDefaultSampleRows, &host.cluster);
            resolve_inferred.push_back(micros(inferred_start, Clock::now()));
            ASSERT_TRUE(inferred.ok()) << inferred.error;

            CpuDevice score_device;
            const auto score_start = Clock::now();
            const zetta::ml::TableScoreResult scored = zetta::ml::score_table(
                host.node, *model, again.table_id, again.contract, score_device);
            score_times.push_back(micros(score_start, Clock::now()));
            ASSERT_TRUE(scored.ok()) << scored.error;
            rows_scored = scored.rows_scored;
        }

        std::cout << "\n=== #956 · where scoring spends its time (median of " << kRepeats
                  << ", " << again.rows << " rows) ===\n"
                  << "resolve_dataset, entity + label named  "
                  << median(resolve_named) / 1000.0 << " ms\n"
                  << "resolve_dataset, label inferred        "
                  << median(resolve_inferred) / 1000.0 << " ms\n"
                  << "score_table                            " << median(score_times) / 1000.0
                  << " ms   (" << rows_scored << " rows scored)\n";
        for (const std::string& note : inferred.notes) std::cout << "  note: " << note << "\n";
        std::cout << std::endl;
    }

    const fs::path out = dir / "run-report.json";
    std::ofstream(out) << artifact.dump(2) << "\n";
    std::cout << "wrote " << out << "\n" << std::endl;

    // ---- the assertions that make it a test, not a print --------------------
    EXPECT_EQ(result.ranked.size(), accounts);
    // The holdout figure is the honest one, and it must still show real discrimination
    // without being suspiciously perfect.
    // The bar is deliberately set at what the model ACTUALLY achieves, not at what we
            // would like. It clears chance comfortably and is nowhere near the ~0.75 a plain
            // logistic regression reaches on the same rows — that gap is the open question
            // recorded in the results report, not something to paper over with a lower
            // threshold and no comment.
    EXPECT_GT(holdout_auc, 0.55) << "no discrimination on unseen accounts";
    EXPECT_LT(holdout_auc, 0.95) << "suspiciously separable on unseen accounts";
    // In-sample always flatters. If it did not exceed the holdout, something is wrong
    // with how one of the two is being computed.
    EXPECT_GE(metrics.at("auc").get<double>(), holdout_auc - 0.02);
    // The AUC arithmetic used for the holdout must reproduce the engine's own in-sample
    // figure when pointed at the training rows. Otherwise the holdout number is not
    // comparable to anything.
    EXPECT_NEAR(recomputed_in_sample, metrics.at("auc").get<double>(), 0.01);
    ASSERT_FALSE(result.drivers.empty()) << "TreeSHAP produced no drivers";
    EXPECT_EQ(result.drivers.size(), encoded_features);
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
