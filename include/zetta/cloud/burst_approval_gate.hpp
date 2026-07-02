#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/provisioner.hpp"

namespace zetta {

class AuditSink;  // include/zetta/governance/audit_event.hpp — where the gate records.

// A configured ceiling on a single provision burst (APR-3 / RSK-9), the cap the
// large-burst approval gate enforces. Two **independent, optional** dimensions; a
// value of 0 on an axis means "unlimited on this axis" (the axis does not gate).
//
//   - max_nodes — how many nodes one provision call may bring up.
//   - max_cost  — the burst's estimated cost (see CostEstimator / default_burst_cost).
//
// A request trips the cap when it exceeds *either* active axis.
struct BurstCap {
    std::size_t max_nodes = 0;  // node-count ceiling for one burst (0 = unlimited)
    double max_cost = 0.0;      // estimated-cost ceiling for one burst (0 = unlimited)

    // Is this axis active (i.e. does it gate)? An axis of 0 is unlimited.
    bool caps_nodes() const { return max_nodes > 0; }
    bool caps_cost() const { return max_cost > 0.0; }

    bool operator==(const BurstCap&) const = default;

    // The JSON value shape:
    //   { "max_nodes": <u64>, "max_cost": <number> }
    // Mirrors schemas/burst_cap.schema.json — keep them in sync.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a malformed cap
    // fails fast instead of silently disabling the gate. Returns std::nullopt on
    // anything malformed: not an object, a wrong-typed field, or a negative number.
    static std::optional<BurstCap> from_json(const nlohmann::json& value);
};

// What the gate computed about a request relative to the cap. Handed to the
// approver so a human / policy can see *why* a burst tripped the cap before
// deciding, and recorded in the audit detail.
struct BurstEstimate {
    std::size_t nodes = 0;       // request.count — the burst's node count
    double cost = 0.0;           // the estimated cost (CostEstimator(request))
    bool over_node_cap = false;  // nodes exceeds an active BurstCap::max_nodes
    bool over_cost_cap = false;  // cost exceeds an active BurstCap::max_cost

    // Does the request exceed the cap on either axis — i.e. does the gate engage?
    bool over_cap() const { return over_node_cap || over_cost_cap; }
};

// A coarse, transparent default cost estimate for a burst, in **relative compute
// units** (not dollars): per node ~ `cores + gpu.throughput_tflops`, times the
// node count. A GPU burst therefore costs far more than a CPU burst — the runaway
// burst cost RSK-9 actually guards against. Replace with real cloud pricing by
// passing a custom CostEstimator when the AWS/GCP backends land (#377 / #378).
double default_burst_cost(const ProvisionRequest& request);

// Decide whether an over-cap burst may proceed. The gate invokes this **only**
// when a request exceeds the cap; returns true to approve (the burst proceeds) or
// false to block it (pending approval). A real deployment wires this to a human
// approval workflow; whichever way it decides, the gate records the decision in
// the audit trail. There is no default approver: an over-cap burst with no
// approver wired is **blocked** (deny-by-default, mirroring the access model).
using BurstApprover =
    std::function<bool(const ProvisionRequest&, const BurstEstimate&)>;

// Estimate a burst's cost from its request. Defaults to default_burst_cost when
// the gate is constructed without one.
using CostEstimator = std::function<double(const ProvisionRequest&)>;

// The large-burst cost-cap approval gate (APR-3 / RSK-9) — a Provisioner that
// **decorates** another one, so it drops in transparently wherever the autoscaler
// / job coordinator holds a `Provisioner&`. It protects against runaway burst
// cost: a request within the cap passes straight through to the inner backend; a
// request exceeding the cap requires approval, is recorded in the audit trail,
// and is **blocked unless approved** — and when blocked the inner backend is
// never called, so no nodes come up and no cost is incurred.
//
// Teardown and listing are not gated (they reduce or merely report cost): they
// delegate straight to the inner backend.
class BurstApprovalGate final : public Provisioner {
public:
    // Wrap `inner` with the cap. `audit` (nullable) is where over-cap decisions
    // are recorded — a null sink means "not audited". `approver` decides over-cap
    // bursts (empty → deny-by-default). `cost` estimates a burst's cost (empty →
    // default_burst_cost). `inner` must outlive the gate.
    BurstApprovalGate(Provisioner& inner, BurstCap cap,
                      AuditSink* audit = nullptr, BurstApprover approver = {},
                      CostEstimator cost = {});

    // Gate a provision request. Within cap → delegate to the inner backend. Over
    // cap → consult the approver (deny-by-default), record the decision in the
    // audit trail, then either delegate (approved) or return a blocked,
    // unsatisfied result (no nodes, a `detail` saying it is pending approval)
    // without touching the inner backend.
    ProvisionResult provision(const ProvisionRequest& request) override;

    // Not gated — delegate to the inner backend.
    bool teardown(const std::string& node_id) override;
    std::vector<ProvisionedNode> provisioned() const override;

    // Compute the estimate for a request against the configured cap (a pure
    // function of the request + cap + cost estimator). Exposed so callers can
    // preview whether a burst would trip the gate before submitting it.
    BurstEstimate estimate(const ProvisionRequest& request) const;

    const BurstCap& cap() const { return cap_; }

private:
    Provisioner& inner_;
    BurstCap cap_;
    AuditSink* audit_;
    BurstApprover approver_;
    CostEstimator cost_;
};

}  // namespace zetta
