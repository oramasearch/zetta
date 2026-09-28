# Decision-policy engine

**Issue:** [#1104](https://github.com/oramasearch/zetta/issues/1104)
**Code:** `zetta::solutions::DecisionPolicyStore`, `decide_table`, and the five
capabilities in `src/core/api/decision_policy_capabilities.cpp`.

The decision-policy engine is the versioned, auditable boundary between a score
and an action. It changes thresholds, tiers, budgets, contact and frequency caps,
eligibility and exclusion rules, autonomy, approvals, and the kill-switch without
retraining a model. It is distinct from `zetta::governance::Policy`, which is the
SQL row-security policy type.

## Replicated records and versioning

Policies and decisions are native replicated records in the node catalog. The
catalog document persists across node restart and reconciles across peers:

```text
decision_policy:<uuid>  -> policy name + immutable ordered versions
decision:<uuid>         -> result + policy id + exact policy version + experiment cell + evidence
```

`create_policy` writes version 1. `update_policy` appends a version and never
edits an earlier config. Full replacements are operator changes. A `tuning`
update accepts only numeric paths declared in the current config's
`tunable_bounds`, checks the minimum and maximum, and rejects unknown or
out-of-range paths. Creation and version changes append
`decision_policy_created` and `decision_policy_updated` audit events.

The policy and decision JSON shapes are validated at the boundary and registered
as `decision_policy` and `decision_record` in the spec index. Their generated
artifacts are `schemas/decision_policy.schema.json` and
`schemas/decision_record.schema.json`.

## Config and evaluation order

A config contains:

| Field | Meaning |
|---|---|
| `kill_switch` | When true, every decision is blocked before any other rule. |
| `threshold` | Minimum score for an action. |
| `tiers[]` | Named score floors and their selected action; the highest matching floor wins. |
| `budget.limit` | Maximum committed cost for one policy version. |
| `contact_cap` | Blocks when the entity's contact count has reached the cap. |
| `frequency_cap` | Maximum actions in the supplied recent window and its window size. |
| `eligibility_rules[]` | Every rule must match. |
| `exclusion_rules[]` | Any matching rule blocks. |
| `autonomy` | `A0`, `A1`, `A2`, or `A3`. |
| `tunable_bounds` | Numeric config paths an agent may tune, with inclusive bounds. |

The evaluator applies the kill-switch, exclusions, eligibility, contact cap,
frequency cap, budget, threshold, and tier in that order. A blocked decision is
still a durable record, including its evidence and policy version.

When an entity supplies an `experiment` reference, the same `decide` call passes
the selected policy action through `ExperimentManager::decide` before persisting
the result. The decision row records the experiment id and allocation `cell`.
A model-driven action assigned to a `holdout` or `below_line` cell is cleared,
marked non-actionable, and stored as blocked. Treatment and exploration cells
retain the policy result. A missing experiment manager or unknown experiment is
a request error rather than an unprotected decision.

Rule operators are `eq`, `neq`, `lt`, `lte`, `gt`, `gte`, `in`, `not_in`, and
`exists`. Rule fields may use dotted paths into the submitted entity object.

Budget use is the cost sum of locally merged `allowed`, `pending_approval`, and
`approved` decisions under the same policy version. Pending A1 work reserves its
cost. Equal-peer replication is convergent rather than linearizable: two peers
deciding concurrently can each act on a partial merged view. Workloads requiring
a strict cluster-wide spending ledger must route a policy's decisions through one
writer until a scoped consensus primitive exists.

## Autonomy and approval

| Class | Decision result |
|---|---|
| `A0` | `manual`; records a recommendation and never authorizes the action. |
| `A1` | `pending_approval`; not actionable until `approve_decision`. |
| `A2` | `allowed` when every hard constraint passes. |
| `A3` | `allowed` when every hard constraint passes. |

`approve_decision` accepts only a pending A1 record. It preserves the original
policy version, marks the record actionable, records approver and time, and emits
`decision_approved` into the immutable audit trail. The latest policy's
kill-switch also blocks approval of work that was already pending.

## Control surface

One registry entry renders each capability over MCP and REST:

| Capability | Kind | Result |
|---|---|---|
| `create_policy` | action | Version 1 and its policy uuid. |
| `get_policy` | read | Current config plus the full immutable version history. |
| `update_policy` | action | A new version from a replacement config or bounded tuning. |
| `decide` | action | One durable decision, or a decisions-table summary. |
| `approve_decision` | action | The approved A1 record. |

Request-time `decide` accepts an `entity` with `entity_id`, `score`, optional
`cost`, `contacts`, `recent_actions`, `experiment`, and arbitrary rule evidence.
Batch mode accepts a database/table plus column mappings and writes full decision
rows to a new table, including `policy_id`, `policy_version`, `experiment`, and
`cell` on every row.

An entity may carry `holdout: true`; batch decisions read it from the
`holdout` column by default (overridable with `holdout_column`). The engine
still selects and records the policy action for counterfactual measurement,
then writes `holdout: true`, `status: "blocked"`, and `actionable: false`.
Action delivery treats this field as a hard stop. This is the input contract
for the holdout manager tracked by #1105; the experiment cell remains in the
decision evidence.

All handlers run on the node owner loop. The evaluator itself has no foreign
runtime or hosted call. The request-time regression measures persisted-decision
p99 over 64 calls and requires it to remain below 50 ms on the native test host.

## Boundaries

- New JSON formats: `DecisionPolicy` and `DecisionRecord`, both schema-as-code.
- New replicated catalog keys: `decision_policy:` and `decision:`.
- Batch mode reads a materialized scored table and writes a replicated output
  table. Partition-owned co-launch is not added by this slice.
- No model weights, training path, foreign runtime, or hosted service is involved.
