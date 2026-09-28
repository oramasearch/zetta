# Executable policy defaults

Refs #1128. `solutions::solution_templates` supplies typed experiment,
guardrail and measurement settings and policy JSON validated by the existing
decision-policy config schema. There is no prose interpreter and no new policy
store: `DecisionPolicyStore::create` persists version 1, and owner edits append
versions through `update_policy`.

S-1 defines high/medium/low score floors of 0.7/0.4/0.0, a high-tier CSM task
with an 86,400-second SLA, a medium-tier retention journey, and low-tier
no-contact. A requested high-tier discount is allowed only through the tier's
explicit alternative-action list: cost above 50 switches its authority to A1.
High and medium tiers also allow an explicit `no_contact` alternative so level
3 can preserve sleeping-dog decisions without changing the entity's risk tier.
The budget is 1,000 currency units per version. Every entity must explicitly
pass the named in-collections, open-complaint and vulnerable exclusions. The
kill switch is off by default.

The default contact limits are 12 lifetime contacts and one action in a rolling
30-day window. Supplied counts cannot erase previously persisted decisions:
the engine takes the larger supplied/recorded count. Pending A1 decisions
reserve capacity; protected holdout decisions do not. Recorded counts span
policy versions. The policy's named action settings may specify authority,
approval-above amount, contact/frequency caps, and SLA. An action absent from
the configured map cannot be referenced by a tier.

S-2 defines hot/standard score floors of 0.7/0.0. Hot leads create an A2 sales
queue task with a one-hour SLA; standard leads create an A2 nurture queue task
with a four-hour SLA. An optional priority queue review is A1. Routing requires
explicit entity, territory, and current rep-capacity eligibility, while named
exclusion and do-not-contact evidence suppresses action. The lifetime cap is
six tasks and the rolling seven-day cap is one. Its deterministic experiment
keeps 10% as permanent holdout and assigns the remainder to `routed`.

S-3 defines strategic/growth/standard predicted-value floors of
1,000/500/0. Strategic entities select an A1 `budget_reallocation`; growth
entities select an A2 `audience_sync`; standard entities select `no_contact`.
Eligibility requires explicit consent, channel eligibility, and margin of at
least 0.2. Closed and do-not-target accounts are excluded. Both active actions
are limited to once per 30 days, the lifetime contact cap is 12, the spending
budget is 10,000 currency units, and the kill switch is off. Every value band
has a 10% holdout; treatment cells are `audience_sync` and
`budget_reallocation`, measured as `net_value` over 365 days. Its five
guardrails cover consent, margin, discount fairness, budget utilization, and
cohort calibration.

S-4 defines priority/standard/defer score floors of 0.7/0.4/0.0. Priority
receivables route to an A2 dialer queue, standard receivables receive an A2
digital treatment, and defer is no contact. A settlement is A1, requires
approval, and cannot spend against the zero starter budget. The policy requires
explicit regulatory eligibility, permitted contact hours, and a non-vulnerable
customer. It caps lifetime contacts at six and rolling contacts at one per
seven days. Its champion/challenger design protects a 10% holdout and splits
treatment between dialer and digital cells.

S-5 defines critical/review/routine interval-uncertainty floors of
0.20/0.05/0.0. Critical and review series create A2 exception tasks with a
one-day SLA; routine series record A0 no-contact. Planner ownership, forecast
freshness and hierarchy consistency are required, while stockout-censored and
locked-override rows are suppressed. The lifetime cap is 52, the rolling
seven-day cap is one, and the default budget is zero. Every tier keeps a 10%
permanent holdout; planner reports omit protected series.

S-8 defines imminent/elevated/monitor hazard floors of 0.65/0.25/0.0. Imminent
assets receive an A0 draft work order; elevated assets receive an A2 inspection
work order; optional scheduling is A1. Eligibility requires an approved asset
class, maintenance window, service safety, and maintenance eligibility.
Lockouts, active work orders, and untrusted sensor evidence suppress action.
The rolling cap is one work order per seven days. Its permanent 10% holdout and
`work_order` cell measure `failure_avoided` over 30 days.

S-6 and S-9 through S-11 intentionally provide minimal starter policies derived from
their catalogue actions. They require explicit eligibility, suppress excluded
entities, set a zero spending budget, use conservative authority for high-stakes
work, and leave the kill switch off. Their module issues refine these defaults.
These numbers are editable configuration, not calibration claims or customer
economics. Experiment defaults provide 10% holdout probability per declared
tier. S-1 treatment cells are `csm_task` and `retention_journey`; its guardrails
cover complaints, discount budget, contact fatigue, and action-execution rate.
S-2 uses the `routed` treatment cell and observes SLA misses, capacity
violations, routing coverage, and action execution.
S-5 uses the `planner_report` treatment cell and observes FVA, interval
coverage, freshness, stockout censoring, and override auditability.

S-7 defines critical/anomaly score floors of 0.8/0.0. Flagged rows create an A2
decision-forum report with a one-day SLA; critical rows may be changed to an A1
executive review with a four-hour SLA. Routing requires explicit eligibility,
forum-owner assignment, and sensitive-driver access, while `suppressed` blocks
action. The exact alert and lifetime cap is 12, with one action per entity per
seven-day window. Its permanent 10% tier holdouts compare with `forum_routed`.
Guardrails cover alert volume, false escalations, sensitive access, causal
claims, decision closure, and action execution. Phased `decision_adopted`
measurement remains descriptive until its module-specific estimator ships.

S-8 observes unsafe and false work orders, missed failures, sensor PSI,
maintenance capacity, and action execution.
Guardrails and outcome/design definitions are typed alongside the policy.

S-12 supplies a complete claim-routing policy. Score floors 0.75 and 0.40
select SIU referral and adjuster assignment; lower scores select fast track.
FNOL, coverage, identity, document-confidence, and fairness-review predicates
must all be explicitly true. Catastrophe and manual-only flags suppress model
routing. Fast-track work above 5,000 cost units moves to A1 approval, while
denial remains an A0 recommendation at every amount. Its three tiers each use
a deterministic 10% holdout, with path-assignment and referral-task treatment
cells measured on 45-day `referral_confirmed` outcomes.

The policy regression checks high/medium/low actions, above-cap A1 approval,
within-cap discounts, missing/true exclusion evidence, and repeated monthly
contacts. Existing exclusion, kill-switch, budget, versioning, replication and
request-time regressions remain applicable. See [decision-policy.md](decision-policy.md)
for the engine's evaluation and equal-peer consistency contract, and
[the registry index](README.md) for the per-module defaults and integration.
