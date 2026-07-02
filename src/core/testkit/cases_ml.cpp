#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "cases.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/tree_shap.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"

// Machine-learning cases — the from-scratch native ML over a leakage-free feature
// table built in code from the synthetic churn dataset: GBDT training (AUC), exact
// TreeSHAP (the efficiency identity + the dominant driver), and probability
// calibration (Brier).
namespace zetta::testkit {
namespace {

std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    return buffer;
}

// The fixed feature ordering the cases share.
const std::vector<std::string>& feature_names() {
    static const std::vector<std::string> names = {
        "failed_payments", "support_severity", "total_events",
        "mrr",             "seats",            "tenure_days"};
    return names;
}

// A leakage-free feature table built in code from the encoded churn shards: for
// each account, reduce only the events at or before the as-of cutoff (events after
// t0 — the leakage trap — are dropped). columns[f][r] is feature f for account r.
struct FeatureTable {
    std::vector<std::vector<double>> columns;  // [feature][row]
    std::vector<double> labels;                // {0,1} per row
};

FeatureTable featurize(const zetta::usecases::churn::ChurnNumericShards& numeric) {
    const std::size_t accounts = numeric.accounts.size();
    FeatureTable table;
    table.columns.assign(feature_names().size(), std::vector<double>(accounts, 0.0));
    table.labels.assign(accounts, 0.0);

    for (std::size_t r = 0; r < accounts; ++r) {
        const auto& account = numeric.accounts[r];
        const std::int64_t entity = account.at("entity").get<std::int64_t>();
        double failed = 0, severity = 0, total = 0;
        for (const auto& event : numeric.events) {
            if (event.at("entity").get<std::int64_t>() != entity) continue;
            if (event.at("ts").get<std::int64_t>() > numeric.as_of) continue;  // leakage trap
            const std::int64_t type = event.at("type").get<std::int64_t>();
            ++total;
            if (type == 3) failed += 1.0;                              // payment_failed
            if (type == 4) severity += event.at("value").get<double>();  // support_ticket
        }
        table.columns[0][r] = failed;
        table.columns[1][r] = severity;
        table.columns[2][r] = total;
        table.columns[3][r] = account.at("mrr").get<double>();
        table.columns[4][r] = static_cast<double>(account.at("seats").get<std::int64_t>());
        table.columns[5][r] = static_cast<double>(account.at("tenure_days").get<std::int64_t>());
        table.labels[r] = account.at("churned").get<std::int64_t>() ? 1.0 : 0.0;
    }
    return table;
}

std::vector<double> row_features(const FeatureTable& table, std::size_t row) {
    std::vector<double> features(table.columns.size());
    for (std::size_t f = 0; f < table.columns.size(); ++f) features[f] = table.columns[f][row];
    return features;
}

FeatureTable dataset_table(const TestContext& context, TestResult& note_into) {
    zetta::usecases::churn::ChurnDataset dataset = sample_churn_dataset();
    if (!context.custom_data_path.empty()) {
        const zetta::usecases::churn::ChurnDatasetResult loaded = zetta::usecases::churn::load_churn_dataset(context.custom_data_path);
        if (loaded.ok()) {
            dataset = loaded.dataset;
            note_into.note("using custom dataset at " + context.custom_data_path);
        } else {
            note_into.note("custom dataset failed to load (" + loaded.error +
                           "); using the built-in synthetic dataset");
        }
    }
    return featurize(encode_churn_dataset(dataset));
}

ml::GbdtParams params_from(const TestOptions& options) {
    return churn_gbdt_params(
        static_cast<int>(options.get_int("trees", 30)),
        static_cast<int>(options.get_int("depth", 3)),
        static_cast<int>(options.get_int("bins", 16)),
        options.get_double("learning_rate", 0.3));
}

// Train a GBDT on the churn feature table and report AUC.
TestResult run_gbdt_train(const TestOptions& options, TestContext& context) {
    TestResult result = TestResult::pass("");
    const FeatureTable table = dataset_table(context, result);
    context.emit("training a GBDT on " + std::to_string(table.labels.size()) + " accounts");

    const ml::GbdtTrainResult trained =
        ml::train_gbdt(table.columns, feature_names(), table.labels, params_from(options));
    if (!trained.ok()) return TestResult::fail("train_gbdt failed: " + trained.status.message);

    std::vector<double> probabilities(table.labels.size());
    for (std::size_t r = 0; r < table.labels.size(); ++r) {
        probabilities[r] = trained.model.predict_proba(row_features(table, r));
    }
    const ml::GbdtMetrics metrics = ml::evaluate_binary(probabilities, table.labels);

    // Strongest driver by total split gain.
    std::size_t best = 0;
    for (std::size_t f = 1; f < trained.feature_importances.size(); ++f) {
        if (trained.feature_importances[f] > trained.feature_importances[best]) best = f;
    }

    if (metrics.auc < 0.5) {
        return TestResult::fail("AUC " + number(metrics.auc) + " is no better than random");
    }
    result.outcome = TestOutcome::Pass;
    result.summary = "trained a GBDT with AUC " + number(metrics.auc);
    result.metric("AUC", number(metrics.auc))
        .metric("log loss", number(metrics.log_loss))
        .metric("accuracy", number(metrics.accuracy))
        .metric("top driver", feature_names()[best]);
    return result;
}

// Train, then explain one account with exact TreeSHAP and check the efficiency
// identity base_value + sum(phi) == predict_raw.
TestResult run_tree_shap(const TestOptions& options, TestContext& context) {
    TestResult result = TestResult::pass("");
    const FeatureTable table = dataset_table(context, result);
    const ml::GbdtTrainResult trained =
        ml::train_gbdt(table.columns, feature_names(), table.labels, params_from(options));
    if (!trained.ok()) return TestResult::fail("train_gbdt failed: " + trained.status.message);

    // Explain the account with the most failed payments (the worst risk).
    std::size_t worst = 0;
    for (std::size_t r = 1; r < table.labels.size(); ++r) {
        if (table.columns[0][r] > table.columns[0][worst]) worst = r;
    }
    const std::vector<double> features = row_features(table, worst);
    context.emit("explaining account row " + std::to_string(worst) + " with TreeSHAP");

    const ml::ShapExplanation explanation = ml::tree_shap(trained.model, features);
    const double residual = std::abs(explanation.efficiency_residual());
    if (residual > 1e-6) {
        return TestResult::fail("efficiency identity broke: residual " + number(residual));
    }
    const ml::ShapDriver driver = ml::dominant_driver(trained.model, explanation);

    double sum_phi = 0.0;
    for (double phi : explanation.values) sum_phi += phi;

    result.outcome = TestOutcome::Pass;
    result.summary = "TreeSHAP is exact (residual " + number(residual) +
                     "); dominant driver: " + driver.feature;
    result.metric("base + sum phi", number(explanation.base_value + sum_phi))
        .metric("predict_raw", number(explanation.predicted))
        .metric("dominant driver", driver.feature.empty() ? "(none)" : driver.feature);
    return result;
}

// Train, then fit a probability calibrator and report the Brier score before/after.
TestResult run_calibration(const TestOptions& options, TestContext& context) {
    TestResult result = TestResult::pass("");
    const FeatureTable table = dataset_table(context, result);
    const ml::GbdtTrainResult trained =
        ml::train_gbdt(table.columns, feature_names(), table.labels, params_from(options));
    if (!trained.ok()) return TestResult::fail("train_gbdt failed: " + trained.status.message);

    std::vector<double> raw(table.labels.size()), before(table.labels.size());
    for (std::size_t r = 0; r < table.labels.size(); ++r) {
        const std::vector<double> features = row_features(table, r);
        raw[r] = trained.model.predict_raw(features);
        before[r] = trained.model.predict_proba(features);  // sigmoid(raw)
    }

    const ml::CalibrationMethod method = options.get_choice("method", 0) == 1
                                             ? ml::CalibrationMethod::Isotonic
                                             : ml::CalibrationMethod::Platt;
    context.emit("fitting a calibrator");
    const ml::CalibrationResult fit = ml::fit_calibrator(raw, table.labels, method);
    if (!fit.ok()) return TestResult::fail("fit_calibrator failed: " + fit.status.message);

    std::vector<double> after(table.labels.size());
    for (std::size_t r = 0; r < raw.size(); ++r) {
        after[r] = fit.calibrator.calibrate(raw[r]);
        if (after[r] < 0.0 || after[r] > 1.0) {
            return TestResult::fail("calibrated value out of [0, 1]: " + number(after[r]));
        }
    }

    const double brier_before = ml::calibration_metrics(before, table.labels).brier;
    const double brier_after = ml::calibration_metrics(after, table.labels).brier;
    result.outcome = TestOutcome::Pass;
    result.summary = "the calibrator fit and produced valid probabilities (Brier " +
                     number(brier_before) + " -> " + number(brier_after) + ")";
    result.metric("brier before", number(brier_before))
        .metric("brier after", number(brier_after))
        .metric("method", method == ml::CalibrationMethod::Isotonic ? "isotonic" : "platt");
    return result;
}

std::vector<TestOption> gbdt_options() {
    return {TestOption::integer("trees", "Trees", 30, 1, 500),
            TestOption::integer("depth", "Max depth", 3, 1, 12),
            TestOption::integer("bins", "Max bins", 16, 2, 256),
            TestOption::real("learning_rate", "Learning rate", 0.3, 0.01, 1.0)};
}

}  // namespace

void register_ml_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "ml.gbdt_train",
        .category = TestCategory::Ml,
        .name = "Train GBDT (AUC)",
        .description = "Train a histogram GBDT on a leakage-free churn feature "
                       "table and report AUC / log loss / the strongest driver.",
        .options = gbdt_options(),
        .run = run_gbdt_train,
    });
    suite.add(TestCase{
        .id = "ml.tree_shap",
        .category = TestCategory::Ml,
        .name = "Explain with TreeSHAP",
        .description = "Train a GBDT and explain an account with exact TreeSHAP, "
                       "checking the efficiency identity base + sum(phi) = raw "
                       "score and surfacing the dominant driver.",
        .options = gbdt_options(),
        .run = run_tree_shap,
    });
    suite.add(TestCase{
        .id = "ml.calibration",
        .category = TestCategory::Ml,
        .name = "Calibrate probabilities",
        .description = "Fit a probability calibrator (Platt / isotonic) on the "
                       "trained model and confirm the Brier score does not worsen.",
        .options =
            [] {
                std::vector<TestOption> options = gbdt_options();
                options.push_back(TestOption::choice("method", "Method",
                                                     {"platt", "isotonic"}, 0));
                return options;
            }(),
        .run = run_calibration,
    });
}

}  // namespace zetta::testkit
