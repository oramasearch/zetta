# Solutions

Runtime foundations shared by deployed solutions: experimental measurement,
action policy, monitoring, audit, and durable evidence that connects model scores
and policy decisions to actions and measured outcomes.

The integrated module implementations are complete for S-1, S-2, S-3, S-4,
S-5, S-6, S-7, S-8, S-9, S-10, S-11, S-12, and B-1 through B-7. This is the scope of
the current landing PR, not completion of the full #1122 catalogue epic.
See [integration.md](integration.md) for merge resolutions and proof.
The [pass-two integration](integration-pass-2.md) records subsequent modules
and their separate static proof.
The [pass-three integration](integration-pass-3.md) records the final module
merges and their static proof.

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
- [S-5.md](S-5.md) — runnable demand forecasting module: prediction intervals,
  planner reports, exception queues, FVA, hierarchy checks, and protected
  holdouts (#1114).
- [S-6.md](S-6.md) — runnable replenishment module: lead-time forecasts,
  constrained plans, ERP actions, bounded release, holdout, and planted-effect
  proof (#1115).
- [S-7.md](S-7.md) — runnable KPI anomaly and driver-analysis module: causal
  seasonal baselines, alert budgets, forum reports, decision logs, protected
  holdouts, and phased measurement (#1116).
- [S-8.md](S-8.md) — runnable predictive-maintenance module: censoring-aware
  failure timing, sensor/leakage refusals, CMMS work orders, holdout, and
  planted-effect proof (#1117).
- [S-9.md](S-9.md) — runnable automated outbound module: consented audiences,
  capped journeys, positive-uplift targeting, holdout, and response proof
  (#1118).
- [S-12.md](S-12.md) — runnable claims triage and fraud-referral module: FNOL
  data, claim paths, A0-only denial, fairness and document checks, holdout, and
  execution (#1121).
- [S-10.md](S-10.md) — runnable request-time fraud decisioning module: graph
  features, case tasks, champion/challenger exploration, and delayed-label
  checks (#1119).
- [S-11.md](S-11.md) — runnable next-best-action module: cross-channel
  arbitration, constrained exploration, logged propensities, consent/contact
  limits, and planted-effect proof (#1120).
- [B-1.md](B-1.md) — runnable dynamic-pricing and discount-governance module:
  identifiable price effects, margin and authority limits, approvals,
  controlled experiments, and planted-effect proof (#1175).
- [B-2.md](B-2.md) — runnable aggregate-first employee-attrition module:
  minimum cohort size, aggregate-only scoring, fairness and approval gates,
  people-leader delivery, and planted-effect proof (#1176).
- [B-3.md](B-3.md) — runnable ticket triage and service automation: native
  language confidence, customer-owned taxonomy routing, explicit abstention,
  approval-only closure, new-type drift, and planted-effect proof (#1177).
- [B-4.md](B-4.md) — runnable document and workflow automation module:
  per-field confidence, owned review queues, validated structured handoff,
  holdout, and planted-effect proof (#1178).
- [B-5.md](B-5.md) — runnable supplier risk and procurement module:
  point-in-time external evidence, post-event leakage refusal, forecast-backed
  flags, bounded procurement actions, and planted-effect proof (#1179).
- [B-6.md](B-6.md) — runnable healthcare-operations module: assistive-only
  scheduling and revenue-cycle work, equity and privacy refusals, PHI
  minimization, protected holdout, and planted-effect proof (#1180).
- [B-7.md](B-7.md) — runnable credit underwriting and limit-management module:
  A0 adverse authority, reason codes, model-risk validation, fairness review,
  uncertainty coverage, holdout, and planted-effect proof (#1181).
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
deployment-card catalogue: this registry describes the S-1 through S-12 and
B-1 through B-7 product modules, the one remaining Band-B sketch, and the
methodology callbacks used to build each module's Pipeline.

## Registry contract

`include/zetta/ml/solution.hpp` defines `Solution` and its supporting value
types. `solutions()` returns 20 stable entries in catalogue order: 19 runnable
catalogue modules and one sketch.

- S-1 through S-12 and B-1 through B-7 have product copy, use case, methodology composition,
  execution order, action and latency classes, autonomy defaults, required data
  roles, policy and measurement templates, guardrails, and four selectable
  capability levels.
- B-8 has `status: "sketch"`, `runnable: false`, and a promotion condition. Its
  data roles describe what promotion would require. It cannot be built; the
  refusal repeats that promotion condition. B-1 through B-7 retain their
  promotion conditions as go-to-market conditions while reporting
  `status: "catalogue"` and `runnable: true`.
- All seven production foundation components are integrated (#1128).
  `runnable` describes the default level's methodology availability;
  `missing_components` lists unavailable methodology callbacks only. It is
  empty for the currently runnable modules. B-8 remains a sketch.

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

### B-2 executable defaults

B-2 defines elevated/watch/baseline aggregate-risk floors of 0.60/0.35/0.0.
Elevated cohorts create an A2 people-leader review, watch cohorts create an A2
aggregate report, and baseline cohorts record A0 no action. An organizational
intervention review is A1. Eligibility requires `group_size >= 5`, people-leader
authorization, completed fairness review, recorded legal approval, and an
explicitly false individual-output flag. Every tier keeps a permanent 10%
holdout and measures `attrition_reduction` over 90 days. See [B-2.md](B-2.md)
for the aggregate-only runtime and five named refusals.

### B-3 executable defaults

B-3 routes tickets at confidence 0.6 and explicitly abstains below that
threshold or for a new ticket type. Level 1 emits an A2 queue task; levels 2–4
add the draft reply. `close_ticket` remains A1 and is never a default tier
action. Customer-owned taxonomy and current-version evidence are required. The
permanent 10% holdout measures `resolution_quality`; six guardrails cover
misroutes, resolution quality, abstention, new types, unapproved closure, and
execution. Its customer-offering promotion condition remains recorded even
though the module is runnable.

S-5 is the complete demand-planning default. Its critical/review/routine tiers
use relative interval uncertainty floors of 0.20/0.05/0.0. Critical and review
series create an A2 exception task with a one-day SLA; routine series record
no contact. Named freshness, ownership, hierarchy, stockout and override rules
control eligibility. The cap is one task per seven days and 52 lifetime tasks.
Each tier has a 10% protected holdout, and the planner report omits held-out
series. Its five observed guardrails cover FVA, interval coverage, freshness,
stockout censoring, and override auditability.

### B-7 executable defaults

B-7 uses 0.70/0.40/0.0 credit-risk floors for underwriter review, limit review,
and approval. Approvals within a 5,000 requested limit are A2; larger approvals
and limit increases require A1. Decline and lower-limit recommendations remain
A0 human-only and are never default score-tier actions. Eligibility requires
verified income, model-validation approval, fairness-review approval, and
reason-code readiness. Every tier has a 10% holdout measuring
`performing_account` over 90 days. See [B-7.md](B-7.md) for its validation and
fair-lending refusals and retained go-to-market condition.

### S-6 executable defaults

S-6 is the complete replenishment default:

| Setting | Executable default |
|---|---|
| Replenish | lead-time demand score ≥ 1; action selected by capability level |
| Actions | draft A0; ERP task A2; release A2 through 100 units and A1 above 100 |
| Hard bounds | quantities above 500 blocked; positive service and budget constraints required |
| Eligibility | `eligible` and `lead_time_confirmed` true; inventory holds and supplier blocks excluded |
| Contact limits | 52 lifetime actions and one action per 30-day window |
| Budget / stop | 10,000 currency units; kill switch off |
| Experiment | permanent 10% holdout; remaining entities enter `replenishment_action` |
| Guardrails | spend, minimum service level, inventory exposure, order volatility, and unapproved over-bound orders |
| Measurement | holdout; named `service_level` outcome per item-location over 30 days |

S-7 is the complete KPI-forum default:

| Setting | Executable default |
|---|---|
| Critical | score ≥ 0.8; decision-forum report, A2, one-day SLA |
| Anomaly | score ≥ 0; decision-forum report, A2, one-day SLA |
| Approval | optional critical-tier executive review; A1, four-hour SLA |
| Alert/contact limits | 12 total and one action per entity per seven-day window |
| Eligibility | `eligible`, `forum_owner_assigned`, and `sensitive_driver_access` must explicitly be true |
| Exclusion | `suppressed` blocks routing |
| Budget / stop | zero monetary spend; kill switch off |
| Experiment | independent 10% holdout probability in each tier; remaining entities enter `forum_routed` |
| Guardrails | alert volume 12; false escalations 0.25; zero access and causal-claim violations; closure 0.9; execution 0.95 |
| Measurement | phased; named `decision_adopted` outcome per decision over 30 days; causal effect and ROI remain non-claimable |

S-7 tasks and its report carry anomaly evidence, candidate drivers, the causal
caveat, forum owner, protected cell, and decision-log requirement through the
shared action-integration path.

S-8 is the complete predictive-maintenance default:

| Setting | Executable default |
|---|---|
| Imminent | hazard ≥ 0.65; A0 draft work order with four-hour SLA |
| Elevated | hazard ≥ 0.25; A2 inspection work order with one-day SLA |
| Monitor | hazard ≥ 0; deliberate no-action recommendation |
| Approval | optional scheduled work order; A1 |
| Work limits | 24 lifetime records and one work order per seven-day window |
| Eligibility | maintenance eligible, approved asset class, maintenance window available, and safe to service must explicitly be true |
| Exclusions | safety lockout, active work order, or untrusted sensor data suppresses work |
| Budget / stop | zero monetary spend; kill switch off |
| Experiment | independent 10% holdout in each risk tier; remaining assets enter `work_order` |
| Guardrails | unsafe/false work orders, missed failures, sensor PSI, capacity utilization, and execution rate |
| Measurement | holdout; named `failure_avoided` outcome per asset over 30 days |

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

S-10 is the complete fraud-decisioning default:

| Setting | Executable default |
|---|---|
| Critical | score ≥ 0.9; A2 fraud-review task, 15-minute SLA |
| Elevated | score ≥ 0.65; A2 request-time challenge |
| Standard | score ≥ 0; A2 approve |
| Approval | optional block action; A1 |
| Caps | one decision per transaction in a one-day window |
| Eligibility | explicit current request-time and graph features, open account, and eligibility evidence |
| Exclusions | named exclusion or manual-authorization-only evidence suppresses model action |
| Budget / stop | zero monetary spend; kill switch off |
| Experiment | independent 10% holdout in each band; champion, challenger, and exploration cells |
| Guardrails | fraud loss, false declines, challenge rate, review load, and action execution |
| Measurement | champion/challenger; named `fraud_loss` outcome over 45 days |

S-10 sends authorization decisions and case tasks through the shared action
outbox. The primary M-1 stage writes and binds a fraud-specific score table;
M-12 supplies the time-bounded graph context.

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

B-1 supplies level-specific price recommendations, review tasks, controlled
experiments, and publication actions. Its policy enforces a margin floor and a
hard authority limit, while experiment and publication actions require A1
approval above the automatic authority limit.

The readout refuses causal/ROI claims for FVA and phased designs until their
module-specific estimators exist.

## Composition and execution

See [executable solution pipelines](executable-pipelines.md) for the input
contract, example, read-backs, failure behavior, and the S-1, S-2, S-3, S-4,
S-5, S-6, S-7, S-8, S-9, S-10, S-11, S-12, and B-1 through B-7 journeys.

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
For B-4, it refuses incomplete per-field thresholds, low-confidence extraction
without an owned review queue, and downstream workflow action before every
required field is validated.

For B-2, it refuses a cohort minimum below five, non-aggregate identity or
outputs, incomplete fairness review, missing legal or works-council approval,
and any destination other than the named people leader. The runtime also
refuses undersized cohort rows, individual identifiers or scores, and later
policy versions that remove the required gates.
For B-6, it refuses a missing protected-group equity review, any policy that
can penalize care access, an unrecorded or unapproved privacy posture, and
model features outside the approved minimum-necessary PHI set. All four levels
run M-1 and bind its healthcare-specific operations score table.

For B-7, it refuses missing adverse-action reasons, incomplete or unsigned
model-risk validation, a missing fairness review, automated adverse authority,
and compositions without M-1 credit scores plus M-17 coverage validation.

For S-6, it refuses missing service or budget constraints, an M-4 horizon
shorter than lead time, unsafe automatic/hard quantity bounds, and a level that
omits M-4, M-16, M-5 when required, or its configured kill switch.

For B-1, it refuses non-identifiable price effects, missing margin or authority
policy limits, approval thresholds above the automatic band, competitor prices
observed after the decision, future price features, and incomplete M-4/M-6/M-16
composition.

For S-7, it refuses causal driver claims, a missing or mismatched alert budget,
and noncausal or nonseasonal baselines. Levels 3 and 4 require rolling-origin
M-4 context with a seasonal-naive baseline.

For S-8, it refuses discarded censoring, missing or future sensor baselines,
and post-failure maintenance leakage. M-5 supplies the decision table; levels
3 and 4 also require M-7.

For S-9, it refuses missing consent/suppression eligibility, absent or disabled
contact caps, a churn-table decision binding, and propensity-only selection
where M-6 uplift is available. Levels 2–4 add M-6, M-8, and M-10 respectively.
For S-10, it refuses immature labels, training populations that erase declined
requests, missing exploration/audit evidence, and request-time feature drift.
All levels require M-1, M-2, and M-12; levels 3 and 4 also require M-5.
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
