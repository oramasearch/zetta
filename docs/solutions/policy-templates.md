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

S-6 defines replenish/no-order tiers. The replenish action is selected by
level: A0 draft at level 1, A2 ERP task at levels 2–3, and A2 bounded release
at level 4. `approval_field: order_quantity` changes release authority to A1
above 100 units, while the 500-unit eligibility bound blocks larger orders.
Eligibility also requires confirmed lead time; inventory holds and supplier
blocks suppress action. The budget is 10,000 currency units, the rolling cap is
one action per 30 days, and the lifetime cap is 52. Its permanent 10% holdout
measures `service_level` against the `replenishment_action` cell.

S-8 defines imminent/elevated/monitor hazard floors of 0.65/0.25/0.0. Imminent
assets receive an A0 draft work order; elevated assets receive an A2 inspection
work order; optional scheduling is A1. Eligibility requires an approved asset
class, maintenance window, service safety, and maintenance eligibility.
Lockouts, active work orders, and untrusted sensor evidence suppress action.
The rolling cap is one work order per seven days. Its permanent 10% holdout and
`work_order` cell measure `failure_avoided` over 30 days.

S-9 defines priority/standard/low response floors of 0.7/0.4/0.0. Priority
entities select an A2 outbound journey with a one-hour SLA; standard entities
select an A2 nurture journey with a one-day SLA; low entities select no contact.
An optional copy review is A1. Current consent, no suppression, and channel
eligibility are mandatory, while suppression, unsubscribe, and do-not-contact
evidence block delivery. The lifetime cap is four contacts. Outbound is limited
to one action in seven days and nurture to one in fourteen. Its deterministic
experiment keeps 10% as permanent holdout and assigns the remainder to
`journey_sent`.

S-10 defines critical/elevated/standard score floors of 0.9/0.65/0.0. Critical
requests create A2 fraud-review tasks with a 15-minute SLA, elevated requests
return A2 challenges, and standard requests return A2 approvals. An optional
block is A1. Eligibility requires current request-time and graph evidence; a
named exclusion or manual-authorization-only flag suppresses action. Its
deterministic experiment protects 10% of every score band and assigns the rest
across champion, challenger, and exploration cells.

S-11 defines recommended/no-positive-value floors of 0.0 and negative one
billion. The recommended tier returns an A2 decision response; an optional
high-stakes review is A1 and no action is A0. Eligibility, consent, and a
remaining cross-channel contact allowance must pass. Exclusion,
do-not-contact, or denied consent suppresses delivery. The lifetime cap is four,
the rolling seven-day cap is one, and the budget is 1,000 currency units. Its
experiment keeps 10% as permanent holdout and divides the rest equally between
M-8 exploit and M-15 explore cells. The explore cell records M-15's actual
selection probability; exploit records one.

B-1 defines high/standard demand floors of 24.0/0.0. Level 1 emits an A0 price
recommendation, level 2 creates an A2 pricing-review task, level 3 creates an A2
controlled experiment, and level 4 publishes an A2 price change. Experiment
and publication actions become A1 above `authority_distance` 0.05. Eligibility
requires pricing approval, a fresh current price, gross margin at least 0.25,
and authority distance at most 0.15; manual-only and legal-hold flags suppress
action. Its two tiers each keep a deterministic 10% holdout and measure 30-day
`net_margin`.

These numbers are editable configuration, not calibration claims or customer
economics. Experiment defaults provide 10% holdout probability per declared
tier. S-1 treatment cells are `csm_task` and `retention_journey`; its guardrails
cover complaints, discount budget, contact fatigue, and action-execution rate.
S-2 uses the `routed` treatment cell and observes SLA misses, capacity
violations, routing coverage, and action execution.
S-5 uses the `planner_report` treatment cell and observes FVA, interval
coverage, freshness, stockout censoring, and override auditability.

S-6 observes replenishment spend, service level, inventory exposure, order
volatility, and unapproved over-bound orders.

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

S-9 uses the `journey_sent` treatment cell and observes unsubscribe, complaint,
consent-violation, contact-frequency, and action-execution metrics.
S-10 observes fraud loss, false declines, challenge rate, review load, and
action execution over a champion/challenger design.
S-11 observes consent,
contact-cap, propensity-log, margin-floor, and action-execution guardrails and
measures `net_value` through uplift cells.
B-1 observes margin-floor, hard authority-band, unapproved-change,
price-feature-leakage, and action-execution guardrails.
Guardrails and outcome/design definitions are typed alongside the policy.

S-12 supplies a complete claim-routing policy. Score floors 0.75 and 0.40
select SIU referral and adjuster assignment; lower scores select fast track.
FNOL, coverage, identity, document-confidence, and fairness-review predicates
must all be explicitly true. Catastrophe and manual-only flags suppress model
routing. Fast-track work above 5,000 cost units moves to A1 approval, while
denial remains an A0 recommendation at every amount. Its three tiers each use
a deterministic 10% holdout, with path-assignment and referral-task treatment
cells measured on 45-day `referral_confirmed` outcomes.

B-2 supplies a complete aggregate workforce-risk policy. Elevated/watch/
baseline floors are 0.60/0.35/0.0 and select an A2 people-leader review, an A2
aggregate report, or A0 no action. The optional organizational-intervention
review is A1. Eligibility requires a cohort size of at least five, explicit
people-leader authorization, passed fairness review, recorded legal approval,
and no individual-level output. Restricted cohorts and individual-output rows
are excluded. Every tier has a permanent 10% holdout; the treatment cell is
`people_leader_review`, and the named 90-day outcome is
`attrition_reduction`. The promotion condition remains catalogue metadata for
go-to-market decisions and does not disable the executable module.
B-7 supplies the credit-policy default. Score floors 0.70, 0.40, and 0.0
select underwriter review, limit review, and approval. Approvals up to a 5,000
requested limit are A2; higher approvals and limit increases require A1.
Decline and lower-limit recommendations are explicit alternatives but remain
A0 human-only and are never selected directly by a score tier. Verified
eligibility and income, model-validation approval, fairness-review approval,
and reason-code readiness must pass. Fraud, bankruptcy, and manual-only holds
block delivery. Each tier keeps a deterministic 10% holdout measured on the
90-day `performing_account` outcome.

B-8 supplies a complete IT and security response policy. Critical, elevated,
and advisory tiers share an exact ten-alert budget. Levels select A0
investigation, A1 containment approval, or single-asset A2 containment inside
an approved playbook; advisory and unflagged rows remain no-action. Owner,
playbook, containment permission, dependency scope, and `affected_assets <= 1`
rules are mandatory. Manual-only, protected-asset, and active-incident flags
block delivery. Every tier has a deterministic 10% holdout, and a boolean kill
switch stops the action path. Five guardrails cover alert volume, containment
scope, unapproved containment, switch bypass, and execution.

The policy regression checks high/medium/low actions, above-cap A1 approval,
within-cap discounts, missing/true exclusion evidence, and repeated monthly
contacts. Existing exclusion, kill-switch, budget, versioning, replication and
request-time regressions remain applicable. See [decision-policy.md](decision-policy.md)
for the engine's evaluation and equal-peer consistency contract, and
[the registry index](README.md) for the per-module defaults and integration.
