# billing — metered usage + plan

The **billing** domain is **metered usage + a plan quota model** for the Studio
Billing/Usage surface (Conflict D). This is an **on-prem / self-hosted** engine:
there is **no payment processor and no invoicing** — a Plan is the quota
contract, and usage is measured live against it. Namespace `zetta::billing`
(`include/zetta/billing/`, `src/core/billing/`).

| Page | What it covers |
|---|---|
| [plan.md](plan.md) | The **Plan** replicated entity (name, tier, numeric quota limits, period) + governed get/create/update; and the live **`system.usage`** metered snapshot the quotas are measured against. |

**The two halves.**

- **`system.usage`** — a single-row, computed-at-query-time snapshot (RULE-7) of
  real consumption: `projects`, `runs`, `jobs`, `models`, `datasets` +
  `dataset_bytes`, `indexes`, `deployments`, `workspace_profiles`, `nodes`. No
  stored counters; it is derived from live `Node` state every read.
- **`Plan`** — the replicated quota/tier config (`plan:<uuid>`), a boundary-
  validated entity modeled on the Model/Workspace-Profile pattern. `quotas` is a
  free-form object of numeric limits (e.g. `max_runs`, `max_storage_bytes`,
  `max_nodes`); the Billing/Usage screen compares `system.usage` against them.
