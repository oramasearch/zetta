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

S-2 and S-4 through S-12 intentionally provide minimal starter policies derived
from their catalogue actions. They require explicit eligibility, suppress
excluded entities, set a zero spending budget, use conservative authority for
high-stakes work, and leave the kill switch off. Their module issues refine
these defaults. These numbers are editable configuration, not calibration
claims or customer economics. Experiment defaults provide 10% holdout
probability per declared tier. S-1 treatment cells are `csm_task` and
`retention_journey`; its guardrails cover complaints, discount budget, contact
fatigue, and action-execution rate. Guardrails and outcome/design definitions
are typed alongside the policy.

The policy regression checks high/medium/low actions, above-cap A1 approval,
within-cap discounts, missing/true exclusion evidence, and repeated monthly
contacts. Existing exclusion, kill-switch, budget, versioning, replication and
request-time regressions remain applicable. See [decision-policy.md](decision-policy.md)
for the engine's evaluation and equal-peer consistency contract, and
[the registry index](README.md) for the per-module defaults and integration.
