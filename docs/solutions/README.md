# Solutions

Runtime foundations shared by deployed solutions: experimental measurement,
action policy, monitoring, audit, and durable evidence that connects model scores
and policy decisions to actions and measured outcomes.

- [S-1.md](S-1.md) — runnable churn and retention module: data roles,
  levels, policy and holdout defaults, action payloads, run sequence, and every
  domain refusal (#1110).
- [S-2.md](S-2.md) — runnable lead and opportunity scoring module: conversion
  labels, capacity-aware CRM queues, SLAs, holdout, and planted-effect proof
  (#1111).
- [S-9.md](S-9.md) — runnable automated outbound module: consented audiences,
  capped journeys, positive-uplift targeting, holdout, and response proof
  (#1118).
- [audit-ledger.md](audit-ledger.md) — the append-only, replicated
  score → decision → action → outcome ledger (#1107), including its API,
  storage, governance, and query contracts.
- [decision-policy.md](decision-policy.md) — versioned score-to-action policy,
  hard constraints, autonomy classes, approvals, batch and request-time decisions
  (#1104).
- [experiments.md](experiments.md) — deterministic holdouts, treatment cells,
  exploration, below-line sampling, exemptions, balance, and uplift rows
  (#1105).
- [monitor.md](monitor.md) — replicated input/score PSI, calibration, closed-window cohort performance, bounded guardrails, breach events, and retrain requests (#1106).
- [point-in-time-snapshots.md](point-in-time-snapshots.md) — replicated snapshot
  tables, row-level as-of joins, and the feature-lineage/blackout gate (#1103).

- [action-integration.md](action-integration.md) — report publication, replicated
  action outbox, webhook and CRM connector plugins, bounded retries,
  acknowledgements, dead letters, holdout suppression, and kill-switch guarded
  closed-loop execution (#1109).

- [roi-readout.md](roi-readout.md) — versioned correctness, aggregate
  incrementality with confidence intervals, customer-supplied ROI, guardrails,
  agent operations, and redacted audit evidence (#1108).

## Solution registry

Issue [#1102](https://github.com/oramasearch/zetta/issues/1102) adds the
catalogue-level registry in `zetta::ml`. It is separate from the older headed
deployment-card catalogue: this registry describes the S-1 through S-12 product
modules, the eight Band-B sketches, and the methodology callbacks used to build
each module's Pipeline.

## Registry contract

`include/zetta/ml/solution.hpp` defines `Solution` and its supporting value
types. `solutions()` returns 20 stable entries in catalogue order:

- S-1 through S-12 have product copy, use case, methodology composition,
  execution order, action and latency classes, autonomy defaults, required data
  roles, policy and measurement templates, guardrails, and four selectable
  capability levels.
- B-1 through B-8 have `status: "sketch"`, `runnable: false`, and a promotion
  condition. Their data roles describe what promotion would require. They
  cannot be built; the refusal repeats that promotion condition.
- All seven production foundation components are integrated (#1128).
  `runnable` describes the default level's methodology availability;
  `missing_components` lists unavailable methodology callbacks only. It is
  empty for the currently runnable S modules. Band-B entries remain sketches.

The product copy contains no pricing or numerical return claims. It says what
problem the module solves and what data roles it needs.

`list_solutions` and `get_solution` are read capabilities on the REST and MCP
surfaces. They are also eligible for an agent's opt-in, read-only capability
list. `pipelines:manage` grants the two solution write operations. The permission
vocabulary test checks both `create_solution` and `run_solution` explicitly,
alongside the owner-authorizes-every-registry-operation invariant.
The public field is `levels`: each element is
`{level: 1..4, name, description}`. Control surfaces post that integer as
`create_solution.level`.

## Portfolio-number mapping

The source catalogue contains three historical M-number schemes. This registry
uses the 21-method portfolio in `docs/ml/methodologies.md` exclusively. The
translations that matter for the catalogue are:

| Historical catalogue label | Portfolio methodology |
|---|---|
| survival / time-to-event M-6 | M-5 |
| causal / uplift M-7 | M-6 |
| anomaly M-5 | M-7 |
| document intelligence M-9 | M-11 |
| optimization M-11 | M-16 |

Language work uses portfolio M-10. The customer agent is a shared foundation
component rather than an extra methodology stage unless a solution explicitly
attaches M-10, as S-9 can at CL-4.

## Executable templates

`policy_template` is validated by the decision-policy engine's config schema.
`experiment_template`, `guardrails`, and `measurement` have typed capability
schemas; prose in `decision_policy_template` / `measurement_design` remains
descriptive only. Execution never interprets that prose.

S-1 is the complete retention default:

| Setting | Executable default |
|---|---|
| High risk | score ≥ 0.7; CSM task, A2, one-day SLA |
| Medium risk | score ≥ 0.4; retention journey, A2, at most one action per 30-day window |
| Low risk | score ≥ 0; deliberate no-contact recommendation, A0 |
| Discount | optional high-tier action; A2 through 50 currency units; above 50 requires A1 approval |
| Contact limits | 12 lifetime contacts and one action per 30-day window; recorded decisions and supplied counts both constrain execution |
| Exclusions | named entity predicates `in_collections`, `open_complaint`, `vulnerable`; each must explicitly be false, so missing evidence blocks |
| Budget / stop | 1,000 currency units per policy version; kill switch off |
| Experiment | independent 10% holdout probability in each high/medium/low tier, 90% treatment |
| Guardrails | maximum complaint fraction 0.01; discount budget 1,000; maximum monthly contact count 1; minimum action-execution rate 0.95 |
| Measurement | holdout; named `retained` outcome per customer over 30 days |

These numerical defaults are editable starter settings, not calibrated risk
cutoffs, business recommendations, prices, or ROI promises. Currency and the
value/investment used in ROI always come from the caller. Owners use
`update_policy` to create later policy versions.

The experiment seed is the low 63 bits of FNV-1a-64 over
`<project UUID>/<solution graph name>` (`project_solution_v1`).
The manager hashes each entity with that fixed seed, independently of its score.
Thus each tier has the same 10% probability, not an exact small-sample quota.
Re-scoring an entity does not move its cell when its risk tier changes. Cell
names include the project/graph scope so unrelated experiments cannot share
ledger evidence accidentally.

S-2 is the complete lead-routing default:

| Setting | Executable default |
|---|---|
| Hot | score ≥ 0.7; sales queue task, A2, one-hour SLA |
| Standard | score ≥ 0; nurture queue task, A2, four-hour SLA |
| Approval | optional hot-tier priority queue review; A1 |
| Contact limits | six lifetime contacts and one action per seven-day window |
| Eligibility | `eligible`, `territory_eligible`, and `rep_capacity_available` must explicitly be true |
| Exclusions | `excluded` and `do_not_contact` suppress routing |
| Budget / stop | zero monetary spend; kill switch off |
| Experiment | independent 10% holdout probability in each score tier; remaining entities enter `routed` |
| Guardrails | zero SLA misses and capacity violations; minimum routing coverage 0.9 and execution rate 0.95 |
| Measurement | holdout; named `converted` outcome per opportunity over 30 days |

S-2 tasks carry the selected queue, owner, territory, capacity evidence, SLA,
and decision cell through the shared action outbox and connector path.

S-9 is the complete automated-outbound default:

| Setting | Executable default |
|---|---|
| Priority | score ≥ 0.7; outbound journey, A2, one-hour SLA |
| Standard | score ≥ 0.4; nurture journey, A2, one-day SLA |
| Low | score ≥ 0; deliberate no-contact recommendation, A0 |
| Approval | optional copy review; A1 |
| Contact limits | four lifetime contacts; one outbound action per seven days; nurture per fourteen days |
| Eligibility | `consent` and `channel_eligible` must be true; `suppressed` must be false |
| Exclusions | `suppressed`, `unsubscribed`, and `do_not_contact` suppress delivery |
| Budget / stop | zero monetary spend; kill switch off |
| Experiment | independent 10% holdout probability in each score tier; remaining entities enter `journey_sent` |
| Guardrails | unsubscribe ≤ 0.01; complaints ≤ 0.005; zero consent violations; contact frequency ≤ 1; execution ≥ 0.95 |
| Measurement | holdout; named `response` outcome per entity over 30 days |

S-9 action payloads carry journey, channel, consent/suppression evidence,
contact counts, SLA, and audience decision cell through the shared action path.

### Minimal defaults

**S-3 through S-8 and S-10 through S-12 are minimal**, marked
`minimal_template: true`. Their module
issues refine the business settings; none is presented as a completed
industry-specific policy. Each uses priority score ≥ 0.7, otherwise no contact,
an explicit `eligible == true` predicate, `excluded == true` suppression,
12 lifetime contacts, one action per 30 days, zero default spend, a 10% holdout,
and the kill switch off. Task SLAs default to one day. Monetary actions therefore
require an owner-approved policy change before spending.

| Module | Priority action / autonomy | Measurement design / outcome | Initial guardrail maximum |
|---|---|---|---|
| S-3 | audience sync / A2 | holdout / net_value | consent violations: 0 |
| S-4 | collections queue / A2 | champion_challenger / net_recovery | complaints: 0 |
| S-5 | planner report / A2 | fva / forecast_error | stale forecasts: 0 |
| S-6 | draft order / A0 | phased / service_level | spend overruns: 0 |
| S-7 | decision-forum report / A2 | phased / decision_adopted | false escalations: 0 |
| S-8 | draft work order / A0 | phased / asset_failure | unsafe actions: 0 |
| S-10 | fraud-review task / A1 | champion_challenger / fraud_loss | false declines: 0 |
| S-11 | eligible-action review / A1 | uplift_cells / net_value | consent violations: 0 |
| S-12 | referral task / A0 | phased / referral_confirmed | automated denials: 0 |

Minimal guardrails are registered but remain unobserved until their metric
producer supplies evidence; absence of an observation is not a healthy reading.
The readout refuses causal/ROI claims for FVA and phased designs until their
module-specific estimators exist.

## Composition and execution

See [executable solution pipelines](executable-pipelines.md) for the input
contract, example, read-backs, failure behavior, and the S-1, S-2, and S-9
journeys.

`compose_solution` selects methodologies at the requested capability level,
calls each registered `build_shape`, tags its nodes, and connects the stages
with control edges. A missing selected shape or runner is refused explicitly.
For S-1, the composed gate also enforces its domain checks and the selected
level's M-5, M-6, or M-8 requirements before the graph is persisted or run.
For S-2, it refuses non-point-in-time conversion labels, post-qualification
predictors, and routing without current owner, territory, and rep-capacity
evidence. Levels 3 and 4 require M-6.
For S-9, it refuses missing consent/suppression eligibility, absent or disabled
contact caps, a churn-table decision binding, and propensity-only selection
where M-6 uplift is available. Levels 2–4 add M-6, M-8, and M-10 respectively.
The composed order is snapshot → methodologies → policy/holdout protection →
actions → ledger → monitor, with a readout definition linked to the graph.
`create_solution` persists real versioned policy, experiment, monitor, graph,
and ROI definition records; `run_solution` uses those records and returns
`preview: false`.

No parallel record system is introduced. Policies, decisions, experiments,
monitors, ledger entries, ROI definitions, snapshot tables, graphs, and Runs use
their existing replicated storage and owner-thread contracts. Capability
schemas and the stored-record schemas remain generated and drift checked.
The internal catalogue source is linked from issue #1102 and is not copied here.
