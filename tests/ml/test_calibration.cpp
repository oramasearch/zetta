#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt.hpp"

// Probability calibration (Platt scaling + isotonic regression, #391): the JSON +
// wire boundaries, the reliability metrics (Brier / ECE), the held-out-split
// improvement each method delivers on a deliberately miscalibrated model, and the
// GbdtModel integration (calibrate_model attaches a map that predict_proba applies
// while preserving the ranking / AUC).
namespace {

using zetta::ml::calibrate_model;
using zetta::ml::Calibrator;
using zetta::ml::calibration_method_from_string;
using zetta::ml::calibration_metrics;
using zetta::ml::CalibrationMethod;
using zetta::ml::CalibrationMetrics;
using zetta::ml::CalibrationResult;
using zetta::ml::evaluate_binary;
using zetta::ml::fit_calibrator;
using zetta::ml::GbdtModel;
using zetta::ml::GbdtParams;
using zetta::ml::to_string;
using zetta::ml::train_gbdt;
using Json = nlohmann::json;

double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

// =====================================================================
// Method names
// =====================================================================

TEST(CalibrationMethodName, RoundTrips) {
    EXPECT_EQ(to_string(CalibrationMethod::Platt), "platt");
    EXPECT_EQ(to_string(CalibrationMethod::Isotonic), "isotonic");
    EXPECT_EQ(calibration_method_from_string("platt"), CalibrationMethod::Platt);
    EXPECT_EQ(calibration_method_from_string("isotonic"),
              CalibrationMethod::Isotonic);
    EXPECT_FALSE(calibration_method_from_string("sigmoid").has_value());
}

// =====================================================================
// JSON boundary
// =====================================================================

TEST(CalibratorJson, PlattRoundTrips) {
    Calibrator calibrator;
    calibrator.method = CalibrationMethod::Platt;
    calibrator.platt_a = 0.42;
    calibrator.platt_b = -0.31;
    std::string error;
    const auto parsed = Calibrator::from_json(calibrator.to_json(), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->method, CalibrationMethod::Platt);
    EXPECT_DOUBLE_EQ(parsed->platt_a, 0.42);
    EXPECT_DOUBLE_EQ(parsed->platt_b, -0.31);
    // The Platt JSON does not carry the isotonic arrays.
    EXPECT_FALSE(calibrator.to_json().contains("iso_bounds"));
}

TEST(CalibratorJson, IsotonicRoundTrips) {
    Calibrator calibrator;
    calibrator.method = CalibrationMethod::Isotonic;
    calibrator.iso_bounds = {-1.0, 0.0, 2.0};
    calibrator.iso_values = {0.1, 0.5, 0.9};
    std::string error;
    const auto parsed = Calibrator::from_json(calibrator.to_json(), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->method, CalibrationMethod::Isotonic);
    EXPECT_EQ(parsed->iso_bounds, (std::vector<double>{-1.0, 0.0, 2.0}));
    EXPECT_EQ(parsed->iso_values, (std::vector<double>{0.1, 0.5, 0.9}));
}

TEST(CalibratorJson, RejectsMalformed) {
    const Json good_platt = Calibrator{.method = CalibrationMethod::Platt,
                                       .platt_a = 1.0,
                                       .platt_b = 0.0}
                                .to_json();
    ASSERT_TRUE(Calibrator::from_json(good_platt).has_value());

    {  // unknown method
        Json bad = good_platt;
        bad["method"] = "spline";
        EXPECT_FALSE(Calibrator::from_json(bad).has_value());
    }
    {  // unknown key
        Json bad = good_platt;
        bad["surprise"] = 1;
        EXPECT_FALSE(Calibrator::from_json(bad).has_value());
    }
    {  // Platt missing a parameter
        Json bad = good_platt;
        bad.erase("platt_b");
        EXPECT_FALSE(Calibrator::from_json(bad).has_value());
    }
    {  // isotonic arrays of unequal length
        Json bad = {{"method", "isotonic"},
                    {"iso_bounds", {0.0, 1.0}},
                    {"iso_values", {0.2}}};
        EXPECT_FALSE(Calibrator::from_json(bad).has_value());
    }
    {  // isotonic bounds not non-decreasing
        Json bad = {{"method", "isotonic"},
                    {"iso_bounds", {1.0, 0.0}},
                    {"iso_values", {0.2, 0.6}}};
        EXPECT_FALSE(Calibrator::from_json(bad).has_value());
    }
    {  // isotonic values not non-decreasing
        Json bad = {{"method", "isotonic"},
                    {"iso_bounds", {0.0, 1.0}},
                    {"iso_values", {0.6, 0.2}}};
        EXPECT_FALSE(Calibrator::from_json(bad).has_value());
    }
    {  // isotonic value out of [0,1]
        Json bad = {{"method", "isotonic"},
                    {"iso_bounds", {0.0, 1.0}},
                    {"iso_values", {0.2, 1.4}}};
        EXPECT_FALSE(Calibrator::from_json(bad).has_value());
    }
}

TEST(CalibratorWire, RoundTrips) {
    Calibrator platt{.method = CalibrationMethod::Platt,
                     .platt_a = 0.33,
                     .platt_b = 0.12};
    const auto platt_back = Calibrator::from_wire(platt.to_wire());
    ASSERT_TRUE(platt_back.has_value());
    EXPECT_DOUBLE_EQ(platt_back->platt_a, 0.33);
    EXPECT_DOUBLE_EQ(platt_back->platt_b, 0.12);

    Calibrator iso;
    iso.method = CalibrationMethod::Isotonic;
    iso.iso_bounds = {-2.0, 0.5, 3.0};
    iso.iso_values = {0.05, 0.4, 0.95};
    const auto iso_back = Calibrator::from_wire(iso.to_wire());
    ASSERT_TRUE(iso_back.has_value());
    EXPECT_EQ(iso_back->iso_bounds, iso.iso_bounds);
    EXPECT_EQ(iso_back->iso_values, iso.iso_values);

    // Empty / malformed wire -> "no calibrator".
    EXPECT_FALSE(Calibrator::from_wire({}).has_value());
    EXPECT_FALSE(Calibrator::from_wire({0.0, 1.0}).has_value());   // Platt needs 3
    EXPECT_FALSE(Calibrator::from_wire({1.0, 2.0, 0.0}).has_value());  // bad length
}

// =====================================================================
// Metrics
// =====================================================================

TEST(CalibrationMetricsTest, PerfectAndWorst) {
    // Perfectly confident + correct -> 0 Brier, 0 ECE.
    const auto perfect = calibration_metrics({0.0, 0.0, 1.0, 1.0}, {0, 0, 1, 1});
    EXPECT_DOUBLE_EQ(perfect.brier, 0.0);
    EXPECT_DOUBLE_EQ(perfect.ece, 0.0);

    // Confidently wrong half the time: Brier 0.5, ECE 0.5 (all in the top bin).
    const auto overconfident =
        calibration_metrics({1.0, 1.0, 1.0, 1.0}, {0, 0, 1, 1});
    EXPECT_DOUBLE_EQ(overconfident.brier, 0.5);
    EXPECT_DOUBLE_EQ(overconfident.ece, 0.5);
}

// =====================================================================
// Held-out improvement on a deliberately miscalibrated model
// =====================================================================

// Build a miscalibrated scenario: a latent score `s` drives the true probability
// sigmoid(s); labels are Bernoulli(sigmoid(s)). The "model" emits an OVERCONFIDENT
// raw score `factor * s` (factor > 1), so its native probability sigmoid(factor*s)
// is pushed toward 0/1 — well-ranked but poorly calibrated. Calibration fit on a
// held-out split should recover sigmoid(s) and lower Brier + ECE on the test split.
struct Miscalibrated {
    std::vector<double> raw_calib, label_calib;  // held-out calibration split
    std::vector<double> raw_test, label_test;    // disjoint test split
    std::vector<double> uncal_prob_test;         // the model's native probabilities
};

Miscalibrated make_miscalibrated(int rows, double factor, unsigned seed) {
    Miscalibrated data;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> score(-3.0, 3.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int i = 0; i < rows; ++i) {
        const double s = score(rng);
        const double label = unit(rng) < sigmoid(s) ? 1.0 : 0.0;
        const double raw = factor * s;
        if (i % 2 == 0) {
            data.raw_calib.push_back(raw);
            data.label_calib.push_back(label);
        } else {
            data.raw_test.push_back(raw);
            data.label_test.push_back(label);
            data.uncal_prob_test.push_back(sigmoid(raw));
        }
    }
    return data;
}

void expect_calibration_improves(CalibrationMethod method) {
    const Miscalibrated data = make_miscalibrated(4000, 3.0, 7);
    const CalibrationResult fit =
        fit_calibrator(data.raw_calib, data.label_calib, method);
    ASSERT_TRUE(fit.ok()) << fit.status.message;

    std::vector<double> cal_prob_test(data.raw_test.size());
    for (std::size_t i = 0; i < data.raw_test.size(); ++i) {
        cal_prob_test[i] = fit.calibrator.calibrate(data.raw_test[i]);
    }

    const CalibrationMetrics before =
        calibration_metrics(data.uncal_prob_test, data.label_test);
    const CalibrationMetrics after =
        calibration_metrics(cal_prob_test, data.label_test);

    // The acceptance: reliability improves on the held-out split.
    EXPECT_LT(after.brier, before.brier) << to_string(method);
    EXPECT_LT(after.ece, before.ece) << to_string(method);

    // Ranking is preserved (a monotone map): AUC does not fall.
    const double auc_before =
        evaluate_binary(data.uncal_prob_test, data.label_test).auc;
    const double auc_after =
        evaluate_binary(cal_prob_test, data.label_test).auc;
    EXPECT_GE(auc_after, auc_before - 0.02) << to_string(method);
}

TEST(CalibrationImproves, Platt) {
    expect_calibration_improves(CalibrationMethod::Platt);
}

TEST(CalibrationImproves, Isotonic) {
    expect_calibration_improves(CalibrationMethod::Isotonic);
}

TEST(CalibrationFit, RejectsBadInput) {
    EXPECT_FALSE(fit_calibrator({}, {}, CalibrationMethod::Platt).ok());  // empty
    EXPECT_FALSE(
        fit_calibrator({0.1, 0.2}, {1.0}, CalibrationMethod::Platt).ok());  // ragged
    EXPECT_FALSE(fit_calibrator({0.1, 0.2, 0.3}, {1.0, 1.0, 1.0},
                                CalibrationMethod::Platt)
                     .ok());  // single class
}

// =====================================================================
// GbdtModel integration — calibrate_model attaches a map predict_proba applies
// =====================================================================

// An overlapping logistic dataset (Bernoulli labels -> interior probabilities, not
// perfectly separable) so the trained model's scores are non-degenerate.
struct Logistic {
    std::vector<std::vector<double>> columns;  // [feature][row]
    std::vector<std::string> names{"x0", "x1"};
    std::vector<double> labels;
};

Logistic make_logistic(int rows, unsigned seed) {
    Logistic data;
    data.columns.assign(2, {});
    std::mt19937 rng(seed);
    std::normal_distribution<double> feature(0.0, 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int i = 0; i < rows; ++i) {
        const double x0 = feature(rng);
        const double x1 = feature(rng);  // noise
        const double label = unit(rng) < sigmoid(1.5 * x0) ? 1.0 : 0.0;
        data.columns[0].push_back(x0);
        data.columns[1].push_back(x1);
        data.labels.push_back(label);
    }
    return data;
}

double auc_over(const GbdtModel& model, const Logistic& data) {
    std::vector<double> probabilities(data.labels.size());
    for (std::size_t r = 0; r < data.labels.size(); ++r) {
        probabilities[r] =
            model.predict_proba({data.columns[0][r], data.columns[1][r]});
    }
    return evaluate_binary(probabilities, data.labels).auc;
}

TEST(CalibrateModel, AttachesAndPreservesRanking) {
    const Logistic train = make_logistic(1200, 1);
    const Logistic holdout = make_logistic(600, 2);  // disjoint draw
    GbdtParams params;
    params.num_trees = 25;
    params.max_depth = 3;
    const auto trained =
        train_gbdt(train.columns, train.names, train.labels, params);
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    GbdtModel model = trained.model;
    ASSERT_FALSE(model.calibration.has_value());

    const double auc_before = auc_over(model, holdout);

    const auto status = calibrate_model(model, holdout.columns, holdout.labels,
                                        CalibrationMethod::Platt);
    ASSERT_TRUE(status.ok()) << status.message;
    ASSERT_TRUE(model.calibration.has_value());

    // predict_proba now routes through the calibrator; the ranking (AUC) is
    // unchanged because Platt is a strictly monotone map.
    const double auc_after = auc_over(model, holdout);
    EXPECT_NEAR(auc_after, auc_before, 1e-9);

    // A calibrated model still round-trips through the GBDT weights JSON Schema,
    // carrying its calibration.
    std::string error;
    const auto reparsed = GbdtModel::from_json(model.to_json(), &error);
    ASSERT_TRUE(reparsed.has_value()) << error;
    ASSERT_TRUE(reparsed->calibration.has_value());
    EXPECT_EQ(reparsed->calibration->method, CalibrationMethod::Platt);

    // An uncalibrated model serializes without the calibration key.
    EXPECT_FALSE(trained.model.to_json().contains("calibration"));
}

TEST(CalibrateModel, RejectsSingleClassHoldout) {
    const Logistic train = make_logistic(400, 3);
    GbdtParams params;
    params.num_trees = 10;
    const auto trained =
        train_gbdt(train.columns, train.names, train.labels, params);
    ASSERT_TRUE(trained.ok());
    GbdtModel model = trained.model;

    // A held-out set with a single label class cannot calibrate.
    std::vector<std::vector<double>> columns{{0.1, 0.2, 0.3}, {0.0, 0.1, 0.2}};
    std::vector<double> labels{1.0, 1.0, 1.0};
    const auto status =
        calibrate_model(model, columns, labels, CalibrationMethod::Platt);
    EXPECT_FALSE(status.ok());
    EXPECT_FALSE(model.calibration.has_value());
}

}  // namespace
