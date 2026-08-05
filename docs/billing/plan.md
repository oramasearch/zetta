# Metered usage + the Plan entity (Conflict D)

The Studio **Billing/Usage** surface is *metered usage measured against a plan*.
Because this is an **on-prem / self-hosted** engine, there is **no payment
processor and no invoice** — the Plan is the **quota contract**, and consumption
is metered **live** from engine state.

Boundary: **no new JSON format beyond the Plan entity's own schema.** `usage` is
a pure read view (no storage).

## `system.usage` — the metered snapshot

`system.usage` (RULE-7) returns **exactly one row**, computed at query time from
live `Node` state — there are no stored counters:

| Column | Source |
|---|---|
| `projects` | `Node::project_names().size()` |
| `runs` | `Node::runs().size()` |
| `jobs` | `Node::job_count()` |
| `models` | `Node::models().size()` |
| `datasets` / `dataset_bytes` | `Node::datasets()` (count + Σ `size_bytes`) |
| `indexes` | `Node::index_count()` |
| `deployments` | `Node::deployment_count()` |
| `workspace_profiles` | `Node::workspace_profile_count()` |
| `nodes` | `Node::cluster_members().size()` |

The Billing/Usage screen reads this and compares each metric against the active
plan's quota.

## The Plan entity

`zetta::billing::Plan` (`include/zetta/billing/plan.hpp`,
`src/core/billing/plan.cpp`) is a **top-level replicated entity** modeled exactly
like [`zetta::ml::Model`](../ml/models.md) /
[`WorkspaceProfile`](../workspace/workspace-profile.md):

| Field | Meaning |
|---|---|
| `name` | the plan's display name (required, non-empty) |
| `tier` | display tier ("free" / "pro" / "enterprise") |
| `quotas` | free-form object of numeric limits (e.g. `max_runs`, `max_storage_bytes`, `max_nodes`) |
| `period_start` | ISO-8601 start of the current billing period |
| `created` / `updated` | ISO-8601 UTC stamps |

- **Replicated state.** `plan:<uuid> → { name, tier, quotas, period_start,
  created, updated }`; on-disk `<plan-uuid>/plan.json`. Reconciles across peers
  (`reconcile_plans`), survives restart on disk and CRDT, boundary-validated
  against `plan_value_schema()` (registered in `boundary_schemas()` →
  `schemas/plan.schema.json`).
- **`system.plans`** read view — one row per plan.
- **Governed capabilities:** `get_plan` (`api:query`), `create_plan` +
  `update_plan` (orchestration, `pipelines:manage` atom).

## Studio

The Settings → Billing/Usage screens read `system.usage` for real consumption and
`get_plan` / `system.plans` for the quota config, rendering usage-against-quota.
Editing the plan (tier / quotas) goes through `create_plan` / `update_plan`.
There is no payment step — this is the on-prem quota surface.
