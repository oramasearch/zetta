#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using namespace std::chrono_literals;

zetta::NodeConfig config_for(const zetta::test::TempDir& directory) {
    zetta::NodeConfig config;
    config.data_dir = directory.path().string();
    config.preflight = false;
    return config;
}

zetta::solutions::MonitorSpec monitor_spec() {
    zetta::solutions::MonitorSpec spec;
    spec.solution = "renewal-risk";
    spec.psi_threshold = 0.2;
    spec.calibration_error_threshold = 0.1;
    spec.calibration_bins = 5;
    spec.retrain_on_breach = true;
    spec.inputs = {
        {"tenure_months", {12.0}, {0.5, 0.5}},
        {"monthly_spend", {50.0, 100.0}, {0.25, 0.5, 0.25}},
    };
    spec.scores = zetta::solutions::DistributionBaseline{
        "risk_score",
        {0.5},
        {0.7, 0.3},
    };
    spec.guardrails = {
        {"complaints", 0.0, 20.0},
        {"cost", 0.0, 100.0},
        {"margin", 0.25, std::nullopt},
        {"fairness", 0.8, 1.25},
        {"fatigue", 0.0, 3.0},
        {"cap_adherence", 0.99, 1.0},
    };
    return spec;
}

const zetta::pubsub::Identity kEngine{
    zetta::pubsub::Address{"service", "monitor-test"},
    {},
};

const zetta::pubsub::Address kSolutionEvents{
    "topic",
    "solution-events",
};

}  // namespace

TEST(SolutionMonitor, ShiftedInputRaisesPsiBreachAndPublishesEvent) {
    zetta::test::TempDir directory;
    zetta::Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    ASSERT_NE(node.pubsub(), nullptr);
    ASSERT_TRUE(node.pubsub()->create_topic(
        kEngine,
        kSolutionEvents,
        {}));
    zetta::pubsub::SubscribeOptions options;
    options.start = zetta::pubsub::StartPosition::Next;
    auto subscription = node.pubsub()->subscribe(
        kEngine,
        zetta::pubsub::Address{"service", "monitor-observer"},
        kSolutionEvents,
        options);
    ASSERT_TRUE(subscription);

    std::string error;
    auto monitor = node.create_monitor(monitor_spec(), &error);
    ASSERT_TRUE(monitor) << error;
    const std::vector<double> shifted(100, 24.0);
    const zetta::solutions::MonitorUpdate update =
        monitor->observe_input("tenure_months", shifted);
    ASSERT_TRUE(update.ok) << update.error;
    ASSERT_EQ(update.new_breaches.size(), 1u);
    EXPECT_EQ(update.new_breaches.front().kind, "input_psi");
    EXPECT_EQ(update.new_breaches.front().metric, "tenure_months");
    EXPECT_GT(update.new_breaches.front().value, 0.2);
    EXPECT_TRUE(update.retrain_requested);

    const auto delivery = subscription->receive(1s);
    ASSERT_TRUE(delivery);
    const nlohmann::json event = nlohmann::json::parse(
        delivery->event.payload.bytes(),
        nullptr,
        false);
    ASSERT_TRUE(event.is_object());
    EXPECT_EQ(event.at("type"), "monitor_breach");
    EXPECT_EQ(event.at("solution"), "renewal-risk");
    EXPECT_EQ(event.at("kind"), "input_psi");
    EXPECT_EQ(event.at("metric"), "tenure_months");
    EXPECT_TRUE(event.at("retrain_requested").get<bool>());

    ASSERT_EQ(monitor->value().breaches.size(), 1u);
    EXPECT_TRUE(monitor->value().inputs.front().breached);
    EXPECT_TRUE(monitor->value().retrain_requested);
    node.stop();
}

TEST(SolutionMonitor, GuardrailOutsideBoundsIsReported) {
    zetta::test::TempDir directory;
    zetta::Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    auto monitor = node.create_monitor(monitor_spec());
    ASSERT_TRUE(monitor);

    const zetta::solutions::MonitorUpdate update =
        monitor->observe_guardrail("cost", 125.0);
    ASSERT_TRUE(update.ok) << update.error;
    ASSERT_EQ(update.new_breaches.size(), 1u);
    const zetta::solutions::MonitorBreach& breach =
        update.new_breaches.front();
    EXPECT_EQ(breach.kind, "guardrail");
    EXPECT_EQ(breach.metric, "cost");
    EXPECT_DOUBLE_EQ(breach.value, 125.0);
    ASSERT_TRUE(breach.maximum);
    EXPECT_DOUBLE_EQ(*breach.maximum, 100.0);

    ASSERT_EQ(monitor->value().breaches.size(), 1u);
    EXPECT_EQ(monitor->value().breaches.front().metric, "cost");
    const zetta::solutions::MonitorUpdate still_breached =
        monitor->observe_guardrail("cost", 140.0);
    EXPECT_TRUE(still_breached.ok);
    EXPECT_TRUE(still_breached.new_breaches.empty());
    const zetta::solutions::MonitorUpdate recovered =
        monitor->observe_guardrail("cost", 80.0);
    EXPECT_TRUE(recovered.ok);
    EXPECT_TRUE(monitor->value().breaches.empty());
    node.stop();
}

TEST(SolutionMonitor, TracksScoresCalibrationAndClosedCohorts) {
    zetta::test::TempDir directory;
    zetta::Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    auto monitor = node.create_monitor(monitor_spec());
    ASSERT_TRUE(monitor);

    const std::vector<double> shifted_scores(20, 0.9);
    EXPECT_TRUE(monitor->observe_scores(shifted_scores).ok);
    const std::vector<zetta::solutions::ScoredLabel> labels = {
        {0.9, true, "enterprise"},
        {0.8, false, "enterprise"},
        {0.2, false, "self-serve"},
        {0.1, true, "self-serve"},
    };
    const auto calibration = monitor->observe_calibration(labels);
    EXPECT_TRUE(calibration.ok) << calibration.error;
    EXPECT_EQ(monitor->value().calibration.samples, 4u);
    EXPECT_TRUE(
        monitor->close_label_window("2026-09", labels).ok);
    ASSERT_EQ(monitor->value().cohorts.size(), 2u);
    EXPECT_EQ(monitor->value().cohorts.front().window, "2026-09");
    EXPECT_FALSE(monitor->value().cohorts.front().cohort.empty());
    EXPECT_EQ(monitor->value().cohorts.front().samples, 2u);
    node.stop();
}

TEST(SolutionMonitor, PersistsAndReplicatesTheMonitorValue) {
    zetta::test::TempDir first_directory;
    zetta::test::TempDir second_directory;
    zetta::cluster::ReplicationManager first_replication{
        zetta::util::new_uuid()};
    zetta::cluster::ReplicationManager second_replication{
        zetta::util::new_uuid()};
    zetta::Node first{config_for(first_directory), &first_replication};
    zetta::Node second{config_for(second_directory), &second_replication};
    ASSERT_TRUE(first.start());
    ASSERT_TRUE(second.start());

    auto monitor = first.create_monitor(monitor_spec());
    ASSERT_TRUE(monitor);
    ASSERT_TRUE(monitor->observe_guardrail("margin", 0.1).ok);
    second_replication.merge(first_replication.save());
    second.tick();

    auto replicated = second.find_monitor("renewal-risk");
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->id(), monitor->id());
    ASSERT_EQ(replicated->value().breaches.size(), 1u);
    EXPECT_EQ(replicated->value().breaches.front().metric, "margin");
    first.tick();
    first.stop();
    second.stop();

    zetta::Node reloaded{config_for(second_directory)};
    ASSERT_TRUE(reloaded.start());
    auto from_disk = reloaded.find_monitor("renewal-risk");
    ASSERT_TRUE(from_disk);
    EXPECT_EQ(from_disk->id(), monitor->id());
    EXPECT_EQ(from_disk->value().breaches.front().metric, "margin");
    reloaded.stop();
}

TEST(SolutionMonitor, RejectsMalformedSpecificationsAndValues) {
    zetta::solutions::MonitorSpec spec = monitor_spec();
    spec.inputs.front().boundaries = {12.0, 6.0};
    std::string error;
    EXPECT_FALSE(zetta::solutions::make_monitor_value(
        std::move(spec),
        "2026-09-28T00:00:00Z",
        &error));
    EXPECT_FALSE(error.empty());

    nlohmann::json malformed = {
        {"solution", "x"},
        {"inputs", nlohmann::json::array()},
    };
    EXPECT_FALSE(zetta::solutions::parse_monitor_value(malformed, &error));
    EXPECT_FALSE(error.empty());
}
