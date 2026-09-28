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

S-9 defines priority/standard/low response floors of 0.7/0.4/0.0. Priority
entities select an A2 outbound journey with a one-hour SLA; standard entities
select an A2 nurture journey with a one-day SLA; low entities select no contact.
An optional copy review is A1. Current consent, no suppression, and channel
eligibility are mandatory, while suppression, unsubscribe, and do-not-contact
evidence block delivery. The lifetime cap is four contacts. Outbound is limited
to one action in seven days and nurture to one in fourteen. Its deterministic
experiment keeps 10% as permanent holdout and assigns the remainder to
`journey_sent`.

S-3 through S-8 and S-10 through S-12 intentionally provide minimal starter policies derived from
their catalogue actions. They require explicit eligibility, suppress excluded
entities, set a zero spending budget, use conservative authority for high-stakes
work, and leave the kill switch off. Their module issues refine these defaults.
These numbers are editable configuration, not calibration claims or customer
economics. Experiment defaults provide 10% holdout probability per declared
tier. S-1 treatment cells are `csm_task` and `retention_journey`; its guardrails
cover complaints, discount budget, contact fatigue, and action-execution rate.
S-2 uses the `routed` treatment cell and observes SLA misses, capacity
violations, routing coverage, and action execution.
S-9 uses the `journey_sent` treatment cell and observes unsubscribe, complaint,
consent-violation, contact-frequency, and action-execution metrics.
Guardrails and outcome/design definitions are typed alongside the policy.

The policy regression checks high/medium/low actions, above-cap A1 approval,
within-cap discounts, missing/true exclusion evidence, and repeated monthly
contacts. Existing exclusion, kill-switch, budget, versioning, replication and
request-time regressions remain applicable. See [decision-policy.md](decision-policy.md)
for the engine's evaluation and equal-peer consistency contract, and
[the registry index](README.md) for the per-module defaults and integration.
