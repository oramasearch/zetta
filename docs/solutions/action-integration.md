# Action integration

Issue [#1109](https://github.com/oramasearch/zetta/issues/1109) supplies the
four action patterns shared by deployed solutions. The implementation is native
C++ and uses the same capability registry for MCP tools and REST routes.

## Patterns

1. **Report to forum.** `publish_report` sends a driver report together with its
   decision log to a forum webhook. The publication record is written before
   delivery, and the idempotency key makes a repeated call return the original
   record.
2. **Task in a system of work.** `enqueue_action_task` writes a task with an
   owner and SLA to the replicated `action_delivery.outbox` table.
   `deliver_action_task` sends it through the selected connector. Generic
   webhook, Salesforce Task, and HubSpot Task connectors implement the same
   request-builder interface.
3. **Decision API.** `decide` remains the request-time policy operation. Its
   persisted decision id is the required input to an action task, so a delivery
   can always identify the policy version and evidence that authorized it.
4. **Bounded closed loop.** `execute_closed_loop` enqueues and attempts one
   policy-approved task. It obeys the task's finite `max_attempts` and checks
   holdout, actionability, and the latest policy kill-switch immediately before
   every outbound request.

The action capabilities are actions in both renderings:

| Capability | REST | Required operation |
|---|---|---|
| `publish_report` | `POST /v1/publish_report` | `orchestration:publish_report` |
| `enqueue_action_task` | `POST /v1/enqueue_action_task` | `orchestration:enqueue_action_task` |
| `deliver_action_task` | `POST /v1/deliver_action_task` | `orchestration:deliver_action_task` |
| `execute_closed_loop` | `POST /v1/execute_closed_loop` | `orchestration:execute_closed_loop` |

Their input and output schemas are also exposed by MCP, OpenAPI, and the spec
index. The durable boundaries are `action_task` and `report_publication`.

## Delivery contract

Every task provides:

- the persisted `decision_id`;
- `owner` and `sla_due_at`;
- connector kind and endpoint;
- an object payload and caller-chosen `idempotency_key`;
- a finite attempt bound, from 1 through 100;
- model, agent, experiment-cell, and currency context for the ledger.

The outbox stores connector kind and endpoint, but never the bearer token.
Credentials are request-local: pass them on the enqueue-and-execute call, or on
the later delivery call. Connector requests carry
`Idempotency-Key: <idempotency_key>`. A successful 2xx response stores its HTTP
status and response body as the acknowledgement. A failed transport or non-2xx
response is recorded and retried until the bound is reached.

When all attempts fail, the outbox row becomes `dead_letter` and is copied once
to `action_delivery.dead_letter`. Both tables are ordinary Node catalog tables;
their rows therefore use the existing table CRDT capture and reconciliation
path. `action_delivery.report_publications` stores report delivery state in the
same way.

The Salesforce plugin maps owner, entity, SLA date, subject, and description to
a Salesforce Task request. The HubSpot plugin maps the same neutral task fields
to HubSpot task properties. Endpoints are supplied by the caller so deployments
can select their own SaaS instance or gateway. Tests use loopback HTTP servers;
the engine never calls a real SaaS service during testing.

## Holdout and kill-switch rules

`DecisionRecord.holdout` is the integration contract for the parallel holdout
manager work in #1105. `decide` copies `entity.holdout` (or the configured batch
`holdout_column`) into the immutable decision row. A held-out row still records
the tier and selected action for evaluation, but its status is `blocked` and
`actionable` is false.

Action delivery applies two defenses:

- enqueue refuses held-out or otherwise non-actionable decisions;
- delivery checks the recorded holdout flag and reads the policy's latest
  version to check `kill_switch` again before each request.

This second check means activating a kill-switch after a task entered the
outbox still changes that task to `blocked` without making the HTTP call.

## Ledger evidence

The first delivery attempt appends one action entry through `record_action`.
Every failed attempt, successful acknowledgement, and dead-letter transition
appends an outcome through `record_outcome`. Report publication follows the same
rule: one action entry and an outcome per attempt or terminal dead letter.

Task rows retain the action ledger id and acknowledgement, allowing an operator
to join operational outbox state to the immutable
[audit ledger](audit-ledger.md). Credentials are absent from both records.

## Ownership and failure behavior

Capability handlers execute through the existing owner-loop bridge. All Node,
catalog, policy, and ledger mutations happen there. Connector implementations
only construct requests; the installed native HTTP transport performs the
exchange.

Schema rejection, a missing decision, a holdout, a non-actionable decision, or
an active kill-switch produces no outbound request. Missing HTTP transport also
fails without changing a pending task into delivered state. Retry and
dead-letter transitions are persisted after each attempt so a caller can read
the latest state by task id.
