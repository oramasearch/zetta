# Audit ledger: score → action → outcome

Issue [#1107](https://github.com/oramasearch/zetta/issues/1107) adds the durable
reward stream used by solutions to connect a scored subject and policy decision
to the action actually taken and its later outcome. Agent actions, human
actions, and deliberate `no_contact` decisions use the same append-only path.
The ledger is separate from the governance audit trail: `system.audit` answers
who called a governed operation, while `system.ledger` carries learning and ROI
facts.

## Record shape

An **action entry** contains:

- subject and score id, numeric score, and decision;
- open-vocabulary action (`no_contact` records an intentional hold);
- actor kind (`agent` or `human`) and actor identity;
- model, policy, and agent versions;
- experiment cell, action cost, and currency; and
- score, decision, action, and server-recorded timestamps.

An **outcome entry** references an action id and contains an outcome, numeric
value, currency, source (`connector` or `manual`), occurrence timestamp, and
server-recorded timestamp. Multiple observations may reference one action;
each observation is a new entry. Joining the outcome to its action restores the
complete score → decision → action → outcome context without copying version or
experiment data into every observation.

Both shapes share the `audit_ledger_entry` JSON Schema. Disk loads, replicated
values, API inputs, generated OpenAPI, and the spec index use schema-as-code
from `audit_ledger_entry_schema()`.

## Capabilities and REST

The capability registry exposes the same contract through MCP and REST:

| Capability | REST | Permission | Result |
|---|---|---|---|
| `record_action` | `POST /v1/record_action` | `orchestration:record_action` | the immutable action entry with its generated id and `recorded_at` |
| `record_outcome` | `POST /v1/record_outcome` | `orchestration:record_outcome` | the immutable outcome entry; an unknown or non-action `action_id` is rejected |
| `list_ledger` | `GET /v1/list_ledger` | `api:query` | every entry in chronological order |

The built-in `pipelines:manage` permission includes both writes. Registry calls
still produce ordinary governance audit events and spans, so authorization and
operational observability remain independent of reward data.

## Persistence and replication

Each entry receives a UUID and is captured first as one `ledger:<uuid>` value in
the Node catalog. The Automerge document replicates it to every node;
owner-thread `Node::tick()` reconciliation validates and materializes the value
under `<data_dir>/ledger/<uuid>.json`. Startup validates those files before
loading them and migration captures older local projections into a newly
attached replication manager.

The public model has no update or delete operation. Recording an outcome
creates a second entry and never modifies its action. Reconciliation is
idempotent by entry id, so repeated gossip cannot replace an already
materialized record.

## Querying and reconstruction

`list_ledger` is the compact control-surface read. SQL provides filtering and
projection through the read-only virtual table:

```sql
SELECT * FROM system.ledger WHERE subject = 'account-42';
SELECT action_id, outcome, value FROM system.ledger WHERE kind = 'outcome';
```

Rows use `ledger_id` plus the fields of their action or outcome shape. A caller
reconstructs a chain by selecting the action whose `ledger_id` matches an
outcome's `action_id`. This retains the exact model, policy, agent, cell, cost,
and timestamps that governed the action and supplies the outcome value used by
later reward and ROI computation.

## Boundaries

- This slice records and queries reward evidence; policy learning and an ROI
  aggregation readout are consumers, not ledger mutations.
- Connector code calls `record_outcome` after it observes a result. Manual
  outcomes use the same capability with `source: "manual"`.
- Timestamps supplied by callers remain evidence timestamps. `recorded_at` is
  assigned by the receiving node and cannot be supplied or changed by a caller.
