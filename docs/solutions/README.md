# Solutions

Runtime foundations shared by deployed solutions: experimental measurement,
action policy, monitoring, audit, and durable evidence that connects model scores
and policy decisions to actions and measured outcomes.

The integrated module implementations are complete for S-1, S-2, S-3, S-4,
S-11, and S-12. S-5 through S-10 retain minimal defaults. This is the scope of
the current landing PR, not completion of the full #1122 catalogue epic.
See [integration.md](integration.md) for merge resolutions and proof.

The workspace integration routes projected solution records through the engine's
[incremental reconciliation](../cluster/solution-reconciliation.md), with
one-tick checks for all added catalog families and existing table-backed state.

- [S-1.md](S-1.md) — runnable churn and retention module: data roles,
  levels, policy and holdout defaults, action payloads, run sequence, and every
  domain refusal (#1110).
- [S-2.md](S-2.md) — runnable lead and opportunity scoring module: conversion
  labels, capacity-aware CRM queues, SLAs, holdout, and planted-effect proof
  (#1111).
- [S-3.md](S-3.md) — runnable customer lifetime value module: continuous-value
  prediction, value-band audience and budget policy, holdouts, actions, and
  domain refusals (#1112).
- [S-4.md](S-4.md) — runnable collections prioritization: regulatory and
  vulnerable-customer checks, treatment tiers, action delivery, holdout, and
  net-recovery readout (#1113).
- [S-12.md](S-12.md) — runnable claims triage and fraud-referral module: FNOL
  data, claim paths, A0-only denial, fairness and document checks, holdout, and
  execution (#1121).
- [S-11.md](S-11.md) — runnable next-best-action module: cross-channel
  arbitration, constrained exploration, logged propensities, consent/contact
  limits, and planted-effect proof (#1120).
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

S-3 is the complete customer-lifetime-value default:

| Setting | Executable default |
|---|---|
| Strategic value | predicted value ≥ 1,000; budget reallocation, A1 approval |
| Growth value | predicted value ≥ 500; audience sync, A2 |
| Standard value | predicted value ≥ 0; deliberate no contact, A0 |
| Eligibility | explicit consent and channel eligibility; margin ≥ 0.2 |
| Exclusions | `do_not_target` and `account_closed` suppress action |
| Limits | 12 lifetime contacts, one action per 30 days, 10,000 currency-unit budget |
| Experiment | independent 10% holdout probability in every value band |
| Guardrails | consent, margin, discount fairness, budget utilization, and cohort calibration |
| Measurement | holdout; named `net_value` outcome per entity over 365 days |

Its thresholds are also editable starter settings. The caller owns the value
definition, horizon, currency, and approval policy.

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

S-4 is the complete collections default:

| Setting | Executable default |
|---|---|
| Priority | score ≥ 0.7; dialer queue, A2, one-day SLA |
| Standard | score ≥ 0.4; digital treatment, A2, one-day SLA |
| Defer | score ≥ 0; deliberate no-contact recommendation, A0 |
| Settlement | optional priority-tier action; A1 approval required; zero starter spending budget |
| Contact limits | six lifetime contacts and one action per seven-day window |
| Eligibility | `collections_eligible`, `regulatory_contact_allowed`, and `within_contact_hours` must be true |
| Protection | `vulnerable` must be false; missing evidence blocks delivery |
| Experiment | 10% holdout; remaining assignments split across dialer and digital cells |
| Guardrails | zero complaints, regulatory violations, and vulnerable-customer contacts; maximum contact frequency 1; minimum execution rate 0.95 |
| Measurement | champion/challenger; named `net_recovery` outcome per receivable over 30 days |

### S-12 executable defaults

S-12 assigns `fast_track`, `adjuster_assignment`, or `siu_referral` using
0.0/0.40/0.75 score floors. All three are governed A2 path tasks;
`denial_recommendation` is A0 human-only and is never a default tier action.
FNOL completeness, coverage, identity, document confidence, and
fairness-review evidence are mandatory. Catastrophe and manual-only flags block
delivery. Fast-track cost above 5,000 requires A1 approval. The deterministic
10% tier holdout measures `referral_confirmed` over 45 days.

S-11 is the complete next-best-action default:

| Setting | Executable default |
|---|---|
| Recommended | policy score ≥ 0; request-time decision response, A2, one-second SLA |
| No positive value | explicit no-action recommendation, A0 |
| Approval | optional high-stakes review, A1, one-hour SLA |
| Contact limits | four lifetime contacts and one action per seven-day window |
| Eligibility | `eligible`, `consent`, and `within_contact_cap` must explicitly pass |
| Exclusions | `excluded`, `do_not_contact`, or denied consent suppress delivery |
| Budget / stop | 1,000 currency units per policy version; kill switch off |
| Experiment | permanent 10% holdout; remaining entities split equally between `exploit` and M-15 `explore` |
| Guardrails | zero consent, cap, missing-propensity, and margin-floor violations; minimum execution rate 0.95 |
| Measurement | uplift cells; named `net_value` outcome per decision over 30 days |

S-11 action payloads carry the selected action and channel, exploit/explore
mode, actual selection probability, predictive propensity, and the explicit
non-causal interpretation through the shared action integration path.

### Minimal defaults

**S-5 through S-10 are minimal**, marked `minimal_template: true`. Their module
issues refine the business settings; none is presented as a completed
industry-specific policy. Each uses priority score ≥ 0.7, otherwise no contact,
an explicit `eligible == true` predicate, `excluded == true` suppression,
12 lifetime contacts, one action per 30 days, zero default spend, a 10% holdout,
and the kill switch off. Task SLAs default to one day. Monetary actions therefore
require an owner-approved policy change before spending.

| Module | Priority action / autonomy | Measurement design / outcome | Initial guardrail maximum |
|---|---|---|---|
| S-5 | planner report / A2 | fva / forecast_error | stale forecasts: 0 |
| S-6 | draft order / A0 | phased / service_level | spend overruns: 0 |
| S-7 | decision-forum report / A2 | phased / decision_adopted | false escalations: 0 |
| S-8 | draft work order / A0 | phased / asset_failure | unsafe actions: 0 |
| S-9 | outbound journey / A2 | holdout / response | complaints: 0 |
| S-10 | fraud-review task / A1 | champion_challenger / fraud_loss | false declines: 0 |

Minimal guardrails are registered but remain unobserved until their metric
producer supplies evidence; absence of an observation is not a healthy reading.
The readout refuses causal/ROI claims for FVA and phased designs until their
module-specific estimators exist.

## Composition and execution

See [executable solution pipelines](executable-pipelines.md) for the input
contract, example, read-backs, failure behavior, and the S-1, S-2, S-3, S-4,
S-11, and S-12 journeys.

`compose_solution` selects methodologies at the requested capability level,
calls each registered `build_shape`, tags its nodes, and connects the stages
with control edges. A missing selected shape or runner is refused explicitly.
For S-1, the composed gate also enforces its domain checks and the selected
level's M-5, M-6, or M-8 requirements before the graph is persisted or run.
For S-2, it refuses non-point-in-time conversion labels, post-qualification
predictors, and routing without current owner, territory, and rep-capacity
evidence. Levels 3 and 4 require M-6.
For S-3, it refuses incomplete short-tenure censoring, an unstated or mismatched
value horizon, and entity accounting that can count one customer more than
once; level 2 also requires M-5.
For S-4, it enforces regulatory contact limits, vulnerable-customer protection,
and point-in-time promise-to-pay labels. All completed modules run these checks before
the graph is persisted and again before execution.
For S-12, it refuses automated denial, missing fairness review, weak document
thresholds, missing claim-score bindings, and incomplete level composition.
For S-11, it refuses missing propensity logs, unenforced consent or
cross-channel contact caps, and any causal claim derived from predictive
response propensity. M-8 and M-15 outputs feed its request-time arbitration.
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
