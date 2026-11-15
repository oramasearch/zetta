// SPDX-License-Identifier: MIT

#include "zetta/data/partition_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

#include <nlohmann/json-schema.hpp>

namespace zetta::data {
namespace {

// A weight below this is treated as "avoid, but still legal" rather than zero —
// a zero would make the load-to-weight ratio infinite and could leave a plan
// with nowhere to put a replica.
constexpr double kMinimumWeight = 0.02;

// How many rows to sample when estimating a dataset's serialized size. Enough to
// smooth out a few long strings, cheap enough to run on every add.
constexpr std::size_t kSizeSampleRows = 64;

// One member's placement weight, with the reason it came out that way.
struct MemberWeight {
    std::string node;
    double weight = 1.0;
    bool measured = false;   // a fresh telemetry sample backed this
    bool nearly_full = false;
};

double clamp_unit(double value, double low, double high) {
    return std::clamp(value, low, high);
}

// The storage factor. **A gate with a taper, not a gradient of used-ness**: a
// node 77% full with hundreds of GB free is a perfectly good target, and
// weighting it 0.23 (the free fraction) would be nonsense — it would also mean a
// node that reports storage at all is always ranked below one that does not.
// Instead a node is neutral until it starts filling, sheds work across the
// approach to the threshold, and is avoided past it.
double storage_factor(const zetta::cluster::NodeMetrics& metrics,
                      const PartitionSchedulerConfig& config, bool& nearly_full) {
    if (!metrics.storage_available) return 1.0;
    if (metrics.storage_percent >= config.nearly_full_storage_percent) {
        nearly_full = true;
        return kMinimumWeight;  // avoided, but still a legal replica target
    }
    if (metrics.storage_percent <= config.storage_taper_percent) return 1.0;
    const double span = std::max(
        config.nearly_full_storage_percent - config.storage_taper_percent, 1.0);
    const double through =
        (metrics.storage_percent - config.storage_taper_percent) / span;
    return clamp_unit(1.0 - 0.75 * through, 0.25, 1.0);
}

// Weigh each member by what its host can currently take on. See the class
// comment in the header for why each factor is here.
//
// **A member with no fresh sample is weighted like a TYPICAL member**, not like
// a perfect one: it takes the mean of the measured weights. Handing an unknown
// node the maximum would systematically starve every node that actually
// reported — the measured ones can only ever score at or below neutral — which
// is how a plan ends up putting an entire dataset on the one node it knows
// nothing about. With nothing measured at all the mean is 1.0 and the whole
// cluster is neutral: the documented even-spread degrade.
std::vector<MemberWeight> weigh_members(const PartitionRequest& request,
                                        const PartitionSchedulerConfig& config) {
    std::map<std::string, const zetta::cluster::NodeMetrics*> samples;
    for (const zetta::cluster::NodeTelemetrySample& sample : request.telemetry) {
        samples.emplace(sample.node_id, &sample.metrics);
    }

    std::vector<MemberWeight> weights;
    weights.reserve(request.members.size());
    double measured_total = 0.0;
    std::size_t measured_count = 0;
    for (const std::string& member : request.members) {
        MemberWeight weight{member, 1.0, false, false};
        const auto found = samples.find(member);
        if (found != samples.end()) {
            const zetta::cluster::NodeMetrics& metrics = *found->second;
            weight.measured = true;
            weight.weight *= storage_factor(metrics, config, weight.nearly_full);
            // Available bandwidth — the ingest's wall clock is set by the
            // slowest uplink, so a saturated link should pull less. Only link
            // *utilization* says anything about headroom; a raw rx/tx rate does
            // not (a fast link at 100 MB/s may be idle), so the rate alone
            // leaves the weight untouched.
            if (metrics.net_utilization_available) {
                weight.weight *=
                    clamp_unit(1.0 - metrics.net_percent / 100.0, 0.05, 1.0);
            }
            // Spare CPU, mildly: decode + Parquet encode are real work, but not
            // the bottleneck a link is. Halved influence, floored at 0.5.
            if (metrics.cpu_percent_available) {
                weight.weight *=
                    clamp_unit(1.0 - metrics.cpu_percent / 200.0, 0.5, 1.0);
            }
            weight.weight = std::max(weight.weight, kMinimumWeight);
            measured_total += weight.weight;
            ++measured_count;
        }
        weights.push_back(std::move(weight));
    }

    const double typical =
        measured_count > 0 ? measured_total / static_cast<double>(measured_count)
                           : 1.0;
    for (MemberWeight& weight : weights) {
        if (!weight.measured) weight.weight = typical;
        weight.weight = std::max(weight.weight, kMinimumWeight);
    }
    return weights;
}

// Members ordered strongest-first, ties on node id, so replica selection is
// deterministic across nodes given the same plan inputs.
std::vector<std::size_t> weight_order(const std::vector<MemberWeight>& weights) {
    std::vector<std::size_t> order(weights.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(),
                     [&weights](std::size_t left, std::size_t right) {
                         if (weights[left].weight != weights[right].weight) {
                             return weights[left].weight > weights[right].weight;
                         }
                         return weights[left].node < weights[right].node;
                     });
    return order;
}

// Round `value` to at most two decimals for the rationale — a weight is a
// judgement, not a measurement, and printing 17 digits of it implies otherwise.
std::string short_number(double value) {
    std::string text = std::to_string(value);
    const std::size_t dot = text.find('.');
    if (dot != std::string::npos && text.size() > dot + 3) text.resize(dot + 3);
    return text;
}

std::string bytes_label(std::size_t bytes) {
    if (bytes >= 1024ull * 1024 * 1024) {
        return short_number(static_cast<double>(bytes) / (1024.0 * 1024 * 1024)) +
               " GiB";
    }
    if (bytes >= 1024ull * 1024) {
        return short_number(static_cast<double>(bytes) / (1024.0 * 1024)) + " MiB";
    }
    if (bytes >= 1024) {
        return short_number(static_cast<double>(bytes) / 1024.0) + " KiB";
    }
    return std::to_string(bytes) + " B";
}

// Collects the first validation failure, so nothing crosses the validator
// boundary as an exception.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "$id": "https://oramasearch.github.io/zetta/schemas/partition_plan.schema.json",
      "title": "zetta.PartitionPlan",
      "description": "The shape a dataset is written in, as the PartitionScheduler decided it (#833): how many partitions, the size each targets, which nodes own each one, and WHY. Capacity-aware placement reads live per-node telemetry, which differs per node, so the plan can no longer be derived independently on each participant — the coordinator computes it once and ships it in the connector-fetch launch payload.",
      "type": "object",
      "required": ["partition_count", "owners", "replication_factor"],
      "additionalProperties": false,
      "properties": {
        "partition_count": { "type": "integer", "minimum": 1, "maximum": 4096 },
        "target_rows_per_partition":  { "type": "integer", "minimum": 0 },
        "target_bytes_per_partition": { "type": "integer", "minimum": 0 },
        "owners": {
          "description": "owners per partition, primary first; one entry per partition",
          "type": "array",
          "items": { "type": "array", "items": { "type": "string", "minLength": 1 } }
        },
        "replication_factor": { "type": "integer", "minimum": 1 },
        "rationale": { "type": "string" }
      }
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

}  // namespace

std::string_view to_string(SourceShape shape) {
    return shape == SourceShape::Fixed ? "fixed" : "flexible";
}

std::uint64_t SourceLayout::total_rows() const {
    std::uint64_t total = 0;
    for (const std::uint64_t rows : unit_rows) total += rows;
    return total;
}

std::size_t PartitionPlan::primary_partitions_of(const std::string& node) const {
    std::size_t owned = 0;
    for (const std::vector<std::string>& partition_owners : owners) {
        if (!partition_owners.empty() && partition_owners.front() == node) ++owned;
    }
    return owned;
}

std::vector<double> PartitionPlan::fetch_weights(
    const std::vector<std::string>& roster) const {
    std::vector<double> weights;
    weights.reserve(roster.size());
    for (const std::string& node : roster) {
        weights.push_back(static_cast<double>(primary_partitions_of(node)));
    }
    return weights;
}

nlohmann::json to_json(const PartitionPlan& plan) {
    return nlohmann::json{
        {"partition_count", plan.partition_count},
        {"target_rows_per_partition", plan.target_rows_per_partition},
        {"target_bytes_per_partition", plan.target_bytes_per_partition},
        {"owners", plan.owners},
        {"replication_factor", plan.replication_factor},
        {"rationale", plan.rationale}};
}

const nlohmann::json& partition_plan_schema() { return schema_document(); }

std::optional<PartitionPlan> partition_plan_from_json(const nlohmann::json& value,
                                                      std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return std::nullopt;
    }

    PartitionPlan plan;
    plan.partition_count = value.at("partition_count").get<std::uint32_t>();
    plan.target_rows_per_partition =
        value.value("target_rows_per_partition", std::size_t{0});
    plan.target_bytes_per_partition =
        value.value("target_bytes_per_partition", std::size_t{0});
    plan.owners = value.at("owners").get<std::vector<std::vector<std::string>>>();
    plan.replication_factor = value.at("replication_factor").get<std::size_t>();
    plan.rationale = value.value("rationale", std::string{});

    // The one cross-field invariant the schema cannot state: a plan whose owner
    // list does not describe every partition would silently under-place, which
    // is the class of bug this whole area exists to make impossible.
    if (plan.owners.size() != plan.partition_count) {
        if (error) {
            *error = "/owners: " + std::to_string(plan.owners.size()) +
                     " owner list(s) for " + std::to_string(plan.partition_count) +
                     " partition(s)";
        }
        return std::nullopt;
    }
    return plan;
}

std::size_t estimate_rows_bytes(const std::vector<nlohmann::json>& rows) {
    if (rows.empty()) return 0;
    const std::size_t stride = std::max<std::size_t>(1, rows.size() / kSizeSampleRows);
    std::size_t sampled = 0;
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < rows.size(); index += stride) {
        bytes += rows[index].dump().size();
        ++sampled;
    }
    if (sampled == 0) return 0;
    return (bytes / sampled) * rows.size();
}

PartitionPlan DefaultPartitionScheduler::plan(const PartitionRequest& request) const {
    PartitionPlan plan;
    plan.target_bytes_per_partition = config_.target_bytes_per_partition;

    const std::size_t node_count = request.members.size();
    // A cache is RF=1 by policy (re-fetchable from its source); otherwise the
    // caller's factor, clamped to the roster so a small cluster still places.
    plan.replication_factor =
        request.cache
            ? std::size_t{1}
            : std::clamp<std::size_t>(request.replication_factor, 1,
                                      std::max<std::size_t>(node_count, 1));

    // --- 1. How many partitions --------------------------------------------
    std::string count_reason;
    if (request.partition_count) {
        // The manual override, honoured EXACTLY (clamped only away from zero,
        // which is not a shape).
        plan.partition_count = std::max<std::uint32_t>(*request.partition_count, 1);
        count_reason = "manual override: " + std::to_string(plan.partition_count) +
                       " partition(s)";
    } else {
        const std::size_t bytes =
            request.approx_bytes > 0
                ? request.approx_bytes
                : request.row_count * kAssumedBytesPerRow;
        const std::size_t target = std::max<std::size_t>(
            config_.target_bytes_per_partition, 1);
        const std::size_t by_size = std::max<std::size_t>(
            (bytes + target - 1) / target, 1);
        const std::size_t floor_for_nodes =
            std::max<std::size_t>(config_.partitions_per_node, 1) *
            std::max<std::size_t>(node_count, 1);

        std::size_t count = std::max(by_size, floor_for_nodes);
        count = std::min<std::size_t>(count, config_.max_partitions);
        // Never more partitions than rows: a partition per row is the degenerate
        // end of "every node gets work" and buys nothing but footers.
        if (request.row_count > 0) count = std::min(count, request.row_count);
        plan.partition_count = static_cast<std::uint32_t>(std::max<std::size_t>(count, 1));

        count_reason =
            "auto: " + bytes_label(bytes) + (request.approx_bytes > 0 ? "" : " (estimated)") +
            " / " + bytes_label(target) + " target = " + std::to_string(by_size) +
            ", floor " + std::to_string(config_.partitions_per_node) + "x" +
            std::to_string(node_count) + " nodes = " + std::to_string(floor_for_nodes) +
            " -> " + std::to_string(plan.partition_count);
        if (request.row_count > 0 && count == request.row_count &&
            request.row_count < std::max(by_size, floor_for_nodes)) {
            count_reason += " (capped at the row count)";
        }
    }

    plan.target_rows_per_partition =
        plan.partition_count > 0
            ? (request.row_count + plan.partition_count - 1) / plan.partition_count
            : 0;

    // --- 2. Where the partitions live ---------------------------------------
    if (node_count == 0) {
        // Nowhere to place: a legal, well-formed plan with no owners, so a
        // standalone caller still gets a count to write with.
        plan.owners.assign(plan.partition_count, {});
        plan.rationale = count_reason + "; no members to place onto";
        return plan;
    }

    const std::vector<MemberWeight> weights = weigh_members(request, config_);
    const std::vector<std::size_t> order = weight_order(weights);
    const std::size_t owners_per_partition =
        std::min(plan.replication_factor, node_count);

    std::vector<double> load(node_count, 0.0);
    plan.owners.reserve(plan.partition_count);
    for (std::uint32_t partition = 0; partition < plan.partition_count; ++partition) {
        // The primary: the node whose load-to-weight ratio would stay lowest.
        // Ties break on roster order, so the choice is deterministic.
        std::size_t primary = 0;
        double best = std::numeric_limits<double>::max();
        for (std::size_t candidate = 0; candidate < node_count; ++candidate) {
            const double ratio =
                (load[candidate] + 1.0) / weights[candidate].weight;
            if (ratio < best) {
                best = ratio;
                primary = candidate;
            }
        }
        load[primary] += 1.0;

        std::vector<std::string> partition_owners{weights[primary].node};
        // Replicas walk the weight order from a per-partition rotation, so they
        // spread across the cluster instead of piling onto the strongest node.
        for (std::size_t step = 0;
             step < node_count && partition_owners.size() < owners_per_partition;
             ++step) {
            const std::size_t candidate = order[(partition + step) % node_count];
            if (candidate == primary) continue;
            partition_owners.push_back(weights[candidate].node);
        }
        plan.owners.push_back(std::move(partition_owners));
    }

    // --- 3. Say why ----------------------------------------------------------
    std::size_t measured = 0;
    std::string avoided;
    for (const MemberWeight& weight : weights) {
        if (weight.measured) ++measured;
        if (weight.nearly_full) {
            avoided += (avoided.empty() ? "" : ", ") + weight.node;
        }
    }
    std::string placement;
    if (measured == 0) {
        placement =
            "no live telemetry for any member — spread evenly (the documented "
            "degrade)";
    } else {
        placement = "weighted by telemetry from " + std::to_string(measured) +
                    " of " + std::to_string(node_count) + " member(s)";
        if (!avoided.empty()) {
            placement += "; avoided nearly-full node(s): " + avoided;
        }
        std::string shares;
        for (std::size_t index = 0; index < node_count; ++index) {
            shares += (shares.empty() ? "" : ", ") + weights[index].node + "=" +
                      short_number(weights[index].weight);
        }
        placement += " [" + shares + "]";
    }
    plan.rationale = std::string(name()) + " scheduler: " + count_reason + "; " +
                     std::to_string(plan.replication_factor) +
                     " owner(s) per partition; " + placement + "; source " +
                     std::string(to_string(request.source.shape)) +
                     (request.source.name.empty() ? ""
                                                  : " (" + request.source.name + ")");
    return plan;
}

const PartitionScheduler& default_partition_scheduler() {
    static const DefaultPartitionScheduler scheduler;
    return scheduler;
}

}  // namespace zetta::data
