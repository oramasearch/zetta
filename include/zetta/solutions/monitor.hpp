// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::solutions {

// A reference population for PSI. `boundaries` contains the n-1 increasing
// cut points for n expected bin shares. Values equal to a boundary stay in the
// lower bin.
struct DistributionBaseline {
    std::string name;
    std::vector<double> boundaries;
    std::vector<double> expected;
};

struct MetricBound {
    std::string name;
    std::optional<double> minimum;
    std::optional<double> maximum;
};

// One scored outcome. `cohort` is used when a label window closes; an empty
// cohort is reported as "all".
struct ScoredLabel {
    double score = 0.0;
    bool label = false;
    std::string cohort;
};

struct MonitorSpec {
    std::string solution;
    std::vector<DistributionBaseline> inputs;
    std::optional<DistributionBaseline> scores;
    double psi_threshold = 0.2;
    double calibration_error_threshold = 0.1;
    std::size_t calibration_bins = 10;
    std::vector<MetricBound> guardrails;
    bool retrain_on_breach = false;
};

struct DistributionState {
    DistributionBaseline baseline;
    std::vector<double> observed;
    double psi = 0.0;
    bool breached = false;
    std::string observed_at;
};

struct CalibrationState {
    std::size_t samples = 0;
    double expected_calibration_error = 0.0;
    double brier_score = 0.0;
    bool breached = false;
    std::string observed_at;
};

struct CohortPerformance {
    std::string window;
    std::string cohort;
    std::size_t samples = 0;
    double accuracy = 0.0;
    double brier_score = 0.0;
    double log_loss = 0.0;
    std::string closed_at;
};

struct GuardrailState {
    MetricBound bound;
    std::optional<double> value;
    bool breached = false;
    std::string observed_at;
};

struct MonitorBreach {
    std::string monitor_id;
    std::string solution;
    std::string kind;
    std::string metric;
    double value = 0.0;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::string observed_at;
    bool retrain_requested = false;
};

struct MonitorValue {
    std::string solution;
    std::vector<DistributionState> inputs;
    std::optional<DistributionState> scores;
    double psi_threshold = 0.2;
    double calibration_error_threshold = 0.1;
    std::size_t calibration_bins = 10;
    CalibrationState calibration;
    std::vector<CohortPerformance> cohorts;
    std::vector<GuardrailState> guardrails;
    std::vector<MonitorBreach> breaches;
    bool retrain_on_breach = false;
    bool retrain_requested = false;
    std::string created;
    std::string updated;
};

struct MonitorUpdate {
    bool ok = false;
    std::string error;
    std::vector<MonitorBreach> new_breaches;
    bool retrain_requested = false;
};

using BreachSink = std::function<void(const MonitorBreach&)>;

// Population stability index over two same-sized distributions. Inputs are
// normalized inside the function. Returns nullopt for empty, mismatched,
// negative, non-finite, or zero-mass distributions.
std::optional<double> population_stability_index(
    std::span<const double> expected,
    std::span<const double> observed);

// Validate and normalize a specification, then make its initial replicated
// value. `created` is used for both timestamps. This is public so other
// solution foundations can validate a monitor before asking a Node to own it.
std::optional<MonitorValue> make_monitor_value(
    MonitorSpec spec,
    std::string created,
    std::string* error = nullptr);

nlohmann::json to_json(const MonitorValue& value);
nlohmann::json to_json(const MonitorBreach& breach);
const nlohmann::json& monitor_value_schema();
bool validate_monitor_value(const nlohmann::json& value,
                            std::string* error = nullptr);
std::optional<MonitorValue> parse_monitor_value(
    const nlohmann::json& value,
    std::string* error = nullptr);

// Per-solution drift and guardrail state (#1106). All mutations capture the
// whole validated value under `monitor:<uuid>` and update monitor.json. A
// breach sink is invoked only when a metric crosses from healthy to breached;
// reconciliation uses apply_value and never invokes it.
class Monitor {
public:
    Monitor(std::string id,
            MonitorValue value,
            std::filesystem::path dir,
            zetta::cluster::ReplicationManager* replication = nullptr,
            BreachSink breach_sink = {});

    const std::string& id() const { return id_; }
    const std::string& solution() const { return value_.solution; }
    const std::string& created() const { return value_.created; }
    const std::string& updated() const { return value_.updated; }
    const MonitorValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    MonitorUpdate observe_input(std::string_view name,
                                std::span<const double> values);
    MonitorUpdate observe_scores(std::span<const double> scores);
    MonitorUpdate observe_calibration(std::span<const ScoredLabel> labels);
    MonitorUpdate close_label_window(std::string window,
                                     std::span<const ScoredLabel> labels);
    MonitorUpdate observe_guardrail(std::string_view name, double value);
    void acknowledge_retrain();

    void apply_value(MonitorValue value);
    nlohmann::json metadata() const;
    void persist_meta() const;

private:
    MonitorUpdate update_distribution(DistributionState& state,
                                      std::span<const double> values,
                                      std::string_view kind);
    MonitorUpdate finish_update(bool was_breached,
                                MonitorBreach breach);
    void rebuild_breaches();
    void capture_and_persist();

    std::string id_;
    MonitorValue value_;
    std::filesystem::path dir_;
    zetta::cluster::ReplicationManager* replication_;
    BreachSink breach_sink_;
};

}  // namespace zetta::solutions
