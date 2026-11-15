// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

// The correctness-gate runner (ENT-5 / FEAT-3, RULE-1). Register a set of checks,
// run them all over an ExecutionGraph, aggregate their outcomes into a
// ValidationReport, and block deploy when any check fails. This is the runner the
// ValidationReport type (validation.hpp) was built to be produced by: it does the
// running; ValidationReport holds the result.
//
// The checks themselves are pluggable (the Check interface below). This file declares
// the runner plus the trivial NonEmptyGraphCheck; the concrete correctness checks
// implement Check and plug in separately without touching the gate. default_gate()
// registers the trivial check, the #309 schema/type + data-leakage / point-in-time
// checks (see graph_checks.hpp), and the #312 entity-resolution eval (TC-4 —
// EntityResolutionCheck, in checks/).
namespace zetta::pipeline {

// One named correctness check: it inspects an ExecutionGraph and reports a single
// CheckResult (name + status + reason). This is the pluggable unit the gate runs.
// Implementations are stateless w.r.t. the graph (a check is a pure function of
// the graph it is handed) but may carry their own configuration.
class Check {
public:
    virtual ~Check() = default;

    // The check's stable identifier, recorded as CheckResult::name. Queryable
    // before the check runs so a gate can list what it will run.
    virtual std::string name() const = 0;

    // Most checks validate one methodology stage at a time. Domain checks that
    // need the manifest and multiple stages opt into the whole composed graph.
    virtual bool evaluates_composed_graph() const noexcept { return false; }

    // Run the check over `graph` and return its outcome. The reason should
    // explain *why* — surfaced to authors, never a stack trace (FEAT-3).
    virtual CheckResult evaluate(const ir::ExecutionGraph& graph) const = 0;
};

// Wrap a name + callable as a Check, so a check that needs no state of its own
// can be registered without declaring a class — handy for one-off checks and
// tests. The callable returns a CheckResult; the wrapper stamps the registered
// `name` onto it before returning, so Check::name() and the result's name can
// never drift.
std::shared_ptr<Check> make_check(
    std::string name,
    std::function<CheckResult(const ir::ExecutionGraph&)> evaluate);

// The trivial check this slice ships: a graph with no steps has nothing to
// deploy, so an empty graph fails the gate. A non-empty graph passes. Concrete
// semantic checks land separately on top of the same interface.
class NonEmptyGraphCheck final : public Check {
public:
    std::string name() const override { return "non_empty_graph"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// The correctness gate: holds an ordered set of registered checks and runs them
// over a graph to produce a ValidationReport.
//
// Policy: the gate blocks deploy on any *failure* (RULE-1 — no deploy without a
// passing gate); warnings pass and do not block. Verdict and block_deploy stay
// separate in the ValidationReport (a stricter gate could one day block on a
// warning) — this gate's policy is the baseline block-on-failure.
class CorrectnessGate {
public:
    // Register a check. Checks run in registration order; the gate co-owns each
    // as a shared handle. Ignores a null check. Returns *this for chaining.
    CorrectnessGate& add_check(std::shared_ptr<Check> check);

    // Run every registered check over `graph`, in registration order, and
    // aggregate the results into a ValidationReport. block_deploy is set true
    // exactly when the aggregate verdict is Fail (some check failed).
    ValidationReport run(const ir::ExecutionGraph& graph) const;

    // The registered checks, in order — the gate hands back shared handles like
    // the rest of zetta.
    const std::vector<std::shared_ptr<Check>>& checks() const { return checks_; }
    std::size_t check_count() const { return checks_.size(); }

private:
    std::vector<std::shared_ptr<Check>> checks_;
};

// The baseline gate callers get for free: the trivial NonEmptyGraphCheck plus the
// concrete #309 checks (SchemaTypeCheck, DataLeakageCheck — see graph_checks.hpp),
// in that order. The #309 checks are opt-in per step, so a graph that carries none
// of their annotations still passes. As further concrete checks land (e.g. #312)
// they are registered here too.
CorrectnessGate default_gate();

}  // namespace zetta::pipeline
