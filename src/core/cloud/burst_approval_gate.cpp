#include "zetta/cloud/burst_approval_gate.hpp"

#include <cstdint>
#include <limits>
#include <sstream>
#include <utility>

#include "zetta/governance/audit_event.hpp"

namespace zetta::cloud {

namespace {

// Accept a JSON integer that is non-negative, whether nlohmann stored it signed
// (a plain `4` literal) or unsigned (what to_json writes). Returns false if it is
// not an integer or is negative — the boundary check for a count/size field. Same
// shape as node_capability.cpp's helper (kept local so the cap parser is
// self-contained).
bool as_nonneg_u64(const nlohmann::json& value, std::uint64_t& out) {
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {  // signed-but-non-negative
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value < 0) return false;
        out = static_cast<std::uint64_t>(signed_value);
        return true;
    }
    return false;
}

// Format a cost figure for an audit detail: drop a trailing ".0" so whole numbers
// read cleanly, but keep fractional precision when it matters.
std::string format_cost(double cost) {
    std::ostringstream out;
    out << cost;  // default formatting trims trailing zeros for us
    return out.str();
}

// Build the human-readable detail recorded with the gate decision: which axis (or
// axes) the burst exceeded, and the outcome.
std::string describe(const BurstCap& cap, const BurstEstimate& estimate,
                     bool approved) {
    std::ostringstream detail;
    detail << "burst of " << estimate.nodes << " node(s)";
    if (estimate.over_node_cap) {
        detail << ", nodes " << estimate.nodes << " > cap " << cap.max_nodes;
    }
    if (estimate.over_cost_cap) {
        detail << ", cost " << format_cost(estimate.cost) << " > cap "
               << format_cost(cap.max_cost);
    }
    detail << (approved ? " — approved" : " — blocked pending approval");
    return detail.str();
}

}  // namespace

double default_burst_cost(const ProvisionRequest& request) {
    // Coarse relative compute units: per node, CPU cores plus GPU throughput, so a
    // GPU burst is weighted far heavier than a CPU burst (the runaway cost we
    // guard against). Honest placeholder until real cloud pricing lands.
    const double per_node = static_cast<double>(request.spec.cores) +
                            request.spec.gpu.throughput_tflops;
    return static_cast<double>(request.count) * per_node;
}

nlohmann::json BurstCap::to_json() const {
    return nlohmann::json{{"max_nodes", max_nodes}, {"max_cost", max_cost}};
}

std::optional<BurstCap> BurstCap::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    BurstCap cap;
    if (value.contains("max_nodes")) {
        std::uint64_t max_nodes = 0;
        if (!as_nonneg_u64(value["max_nodes"], max_nodes)) return std::nullopt;
        if (max_nodes > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        cap.max_nodes = static_cast<std::size_t>(max_nodes);
    }
    if (value.contains("max_cost")) {
        if (!value["max_cost"].is_number()) return std::nullopt;
        cap.max_cost = value["max_cost"].get<double>();
        if (cap.max_cost < 0.0) return std::nullopt;
    }
    return cap;
}

BurstApprovalGate::BurstApprovalGate(Provisioner& inner, BurstCap cap,
                                     zetta::governance::AuditSink* audit, BurstApprover approver,
                                     CostEstimator cost)
    : inner_(inner),
      cap_(std::move(cap)),
      audit_(audit),
      approver_(std::move(approver)),
      cost_(std::move(cost)) {}

BurstEstimate BurstApprovalGate::estimate(const ProvisionRequest& request) const {
    BurstEstimate estimate;
    estimate.nodes = request.count;
    estimate.cost = cost_ ? cost_(request) : default_burst_cost(request);
    estimate.over_node_cap = cap_.caps_nodes() && estimate.nodes > cap_.max_nodes;
    estimate.over_cost_cap = cap_.caps_cost() && estimate.cost > cap_.max_cost;
    return estimate;
}

ProvisionResult BurstApprovalGate::provision(const ProvisionRequest& request) {
    const BurstEstimate est = estimate(request);

    // Within the cap: the common path. Pass straight through, unaudited — the gate
    // governs only the exception (an over-cap burst), so a steady stream of small
    // autoscaler provisions does not flood the audit trail.
    if (!est.over_cap()) return inner_.provision(request);

    // Over the cap: the gate engages. Deny-by-default — without an approver an
    // over-cap burst is blocked.
    const bool approved = approver_ && approver_(request, est);

    // Record the decision in the audit trail (RSK-9): a provision burst hit the
    // cost cap. `allowed` carries the outcome (approved vs. blocked).
    if (audit_) {
        audit_->record_audit_event(
            {zetta::governance::AuditAction::ProvisionRequested, /*actor=*/{},
             /*target=*/std::to_string(est.nodes) + " nodes", approved,
             describe(cap_, est, approved), /*time=*/{}});
    }

    if (approved) return inner_.provision(request);

    // Blocked: do not touch the inner backend — nothing comes up, no cost is
    // incurred. The result says so for the caller (autoscaler / coordinator).
    ProvisionResult blocked;
    blocked.satisfied = false;
    blocked.detail = describe(cap_, est, /*approved=*/false);
    return blocked;
}

bool BurstApprovalGate::teardown(const std::string& node_id) {
    return inner_.teardown(node_id);
}

std::vector<ProvisionedNode> BurstApprovalGate::provisioned() const {
    return inner_.provisioned();
}

}  // namespace zetta::cloud