# Action integration

Issue [#1109](https://github.com/oramasearch/zetta/issues/1109) supplies the
four action patterns shared by deployed solutions. The implementation is native
C++ and uses the same capability registry for MCP tools and REST routes.

## Patterns

1. **Report to forum.** `publish_report` sends a driver report together with its
   decision log to a forum webhook. The publication record is written before
   delivery, and a repeated call with the same actor and specification returns
   the original record.
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

## Idempotency and credential binding

[#1410](https://github.com/oramasearch/zetta/issues/1410) binds each key to the
enqueuing actor and a SHA-256 digest of the complete non-secret specification.
The digest uses canonical JSON with sorted object keys and includes the
decision, owner, SLA, connector kind, exact endpoint, timeout, payload, attempt
limit, and ledger context. Report publications bind the run, title, driver
report, decision log, webhook endpoint, timeout, attempt limit, and all version,
cell, and currency fields. Delivery status, timestamps, acknowledgements, and
attempt counters can change without changing the binding.

An enqueue or closed-loop retry with a different actor or specification fails
before attaching the supplied token to an HTTP request. Direct delivery by
task id also requires the enqueuing actor and verifies the stored specification
against its digest. Report preparation applies the same checks before building
the request, covering both direct publication and a solution run's deferred
delivery. Checks also apply to delivered and dead-letter records. Endpoint and
actor strings are compared exactly; no URL or identity normalization merges
distinct caller inputs. Native callers must provide a nonempty actor; the API
uses its caller id, falling back to `local` when that id is empty.

Tokens remain request-local and are excluded from the digest so the same actor
can rotate credentials for an unchanged specification. Actor and digest fields
replicate with the existing table row. These are consistency checks on the
authorized action APIs, not authentication of arbitrary catalogue writes.
Raw SQL writers with grants on `action_delivery.outbox` or
`action_delivery.report_publications` can replace the stored actor and digest.
Protecting direct task-id delivery against those writers needs a separate
authority boundary: reserve these internal tables from ordinary SQL writes, or
keep authoritative bindings outside the writable tables. Closed-loop calls
still compare the incoming specification with the stored contents before using
fresh credentials.
Concurrent peers retain the existing eventual-consistency model; the binding
does not add cluster-wide exactly-once delivery.

Legacy rows without bindings remain readable for inspection but cannot be
reused or delivered. Recreate pending work under a new key with a known actor
and the intended specification; the runtime never assigns an owner to an old
row from the caller who first retries it. The existing `action_task` and
`report_publication` JSON boundaries gain optional `actor` and `spec_digest`
fields, and publications also store `timeout_seconds` (legacy default 30).
Upgrade every node serving action APIs before relying on these checks. Older
versions reject the new record fields and still permit unsafe reuse of legacy
rows.

The Salesforce plugin maps owner, entity, SLA date, subject, and description to
a Salesforce Task request. The HubSpot plugin maps the same neutral task fields
to HubSpot task properties. Endpoints are supplied by the caller so deployments
can select their own SaaS instance or gateway. Tests use loopback HTTP servers;
the engine never calls a real SaaS service during testing.

Because the endpoint is the author's choice, delivery (tasks and report
publications alike) may reach only **public addresses** (#1394): the host is
resolved at delivery, loopback, private, link-local, metadata and reserved
addresses are refused unless the node allows their class
(`--action-allow-destinations` / `ZETTA_ACTION_ALLOW_DESTINATIONS`), and the
connection goes to the address that was checked. A refused attempt records
`destination refused: <host> resolves to <address>, a <class> address ...` as
its error and counts toward the attempt bound like any transport failure. See
[../governance/destination-policy.md](../governance/destination-policy.md).

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

**A solution run's report is delivered off the owner loop**
([lake#801](https://github.com/oramasearch/lake/issues/801)). The owner loop
serves every capability call in turn, so a network round-trip made on it stops
the whole workspace answering for as long as the endpoint takes. `run_solution`
therefore splits the report into three steps:

1. `ActionRuntime::prepare_report` (owner): validate, persist the
   `report_publication`, append its ledger action, and build the request. It
   sends nothing. A publication already delivered or dead-lettered under the
   same actor, specification, and idempotency key returns with no request.
2. `send_report_request` (the run's coroutine, never the owner): the installed
   async transport, or the synchronous one run on a runtime pool worker.
3. `ActionRuntime::record_report_attempt` (owner): count the attempt, record
   its outcome in the ledger, persist, and settle the publication as
   `delivered`, or as `dead_letter` once `max_attempts` is spent.

The Run stays `running` while the report is in flight, logs each failed attempt
as a warning, and fails with `driver report delivery was not acknowledged after
N attempt(s): <last reason>` (or `planner report …` for S-5) when no attempt is
acknowledged. Each attempt is bounded by the request's `timeout_seconds` (30 s
by default), which covers the connect and TLS handshake as well as the exchange
(see [native HTTP transport](../api/native-http-transport.md)), so a run with
an unreachable endpoint fails within `max_attempts × timeout_seconds` without
stopping the workspace. `publish_report` called directly still runs all three
steps on the calling thread.

**`deliver_action_task` and `execute_closed_loop` send off the owner loop**
(#1406, LSR-173). Both are `blocks` entries, so REST and MCP call them on the
blocking pool after the registry's owner-thread authorization.
`ActionRuntime::deliver_task` runs each step that touches node state on the
owner through `Node::run_on_owner`, and only the attempt itself on the calling
thread:

1. read the task, check its binding and status, and read its decision (owner);
2. per attempt, recheck the kill-switch and holdout and append the ledger
   action once (owner);
3. build the request and send it through the transport (calling thread);
4. count the attempt, record its outcome in the ledger and persist the task
   (owner);
5. when attempts run out, settle it as `dead_letter` and copy it to the
   dead-letter table (owner).

`execute_closed_loop` enqueues on the owner first. A stalled endpoint holds
one blocking-pool thread for up to `max_attempts × timeout_seconds`; the owner
loop keeps answering every other call. Called on the owner itself (the headed
app, an in-process caller), `run_on_owner` is inline and the call behaves as
before.

This is the blocking-pool form of the design on #1406, not a coroutine: the
registry has no awaitable handler, and a coroutine handler would hold
`Registry::call`'s thread-local actor and surface scopes across a suspension.

Still on the owner loop: the per-decision action deliveries made inside
`finish_solution_run`, and the `publish_report` capability. Each attempt is
bounded by `timeout_seconds`, but those calls still hold the owner for up to
`max_attempts × timeout_seconds`
([#1365](https://github.com/oramasearch/zetta/issues/1365)).

Schema rejection, a missing decision, a holdout, a non-actionable decision, or
an active kill-switch produces no outbound request. Missing HTTP transport also
fails without changing a pending task into delivered state. Retry and
dead-letter transitions are persisted after each attempt so a caller can read
the latest state by task id.
