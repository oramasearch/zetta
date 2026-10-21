# Step authority: a graph step runs as the run's principal

Issue: [#843](https://github.com/oramasearch/zetta/issues/843) (replaces #25 API-5 and #467 CTL-1/TB-2).
Code: [`include/zetta/pipeline/step_authority.hpp`](../../include/zetta/pipeline/step_authority.hpp),
[`src/core/pipeline/step_authority.cpp`](../../src/core/pipeline/step_authority.cpp).
Tests: [`tests/pipeline/test_step_authority.cpp`](../../tests/pipeline/test_step_authority.cpp).

## The gap this closes

The capability check ran once, at submission: a caller needed the right to call `run`
(`orchestration:run`), or `orchestration:start_run` in the console, and every step of the
pipeline then executed with the node's full authority. A principal allowed to start a run
could therefore make a connect step read a table it may not read, or have a model, serve or
act step do what its roles do not allow.

Now each step is authorized **when it is reached**, for the identity that started the run,
**deny-by-default**:

1. Each step kind names the right it exercises (`step_operation`).
2. The run's principal must hold that right. A user is checked against its roles
   (`Node::authorize_user`), an AccessKey against its capabilities (`Node::authorize`). An
   unknown or disabled identity is denied.
3. A refused step does not execute. The run ends `failed` on that step (fail-fast, as for any
   failed step), with the denial in its log and in the API result's `error`.
4. Every decision for a named principal, granted or denied, is one `data_access` event on the
   existing audit trail (`Node::record_audit_event`). There is no new audit action and no new
   JSON boundary.

## The rights each step needs

The rights are the existing permission vocabulary's, so no new permission atom is needed:

| Step kind | Right | Granted by |
|---|---|---|
| connect | `data:read`, or `sql:read` on the table its `source` + `table` attributes name | `sql:read` (flat, or `ON TABLE`) |
| clean | `orchestration:create_dataset_from_query` (it derives a dataset from data the run already read, and reaches no new source) | `pipelines:manage` |
| model | `orchestration:train_model` | `pipelines:manage` |
| index | `orchestration:create_index` | `pipelines:manage` |
| serve | `data:predict` (it answers from a model; exposing an endpoint stays the separate `expose_model` call) | `serving:read` |
| act | `orchestration:deliver_action_task` | `pipelines:manage` |

The built-in `admin` role holds every right, so an admin's runs are unchanged. A custom role
that may call `run` now also needs the rights of the steps its pipelines contain.

## The audit record

| Field | Value |
|---|---|
| `action` | `data_access` |
| `actor` | the principal (user id or AccessKey id) |
| `target` | `step:<kind>:<step name>` |
| `allowed` | the decision |
| `detail` | `<kind> step '<name>' needs <domain>:<action>[ on <db>.<table>][ · run <id>] · granted` or `· denied: <reason>` |

## Where it is enforced

| Path | Principal | How |
|---|---|---|
| `GraphExecutor::execute` | `GraphExecutor::run_as(RunPrincipal)` | before each step, on the owner thread |
| `Node::run_graph(graph, project, graph_id, principal)` | the argument | a `GraphExecutor` run as the principal |
| API `run` (also reached by `agent_run` and the training helpers that call it), structural pipelines | the call's acting identity | `Node::run_graph` with the principal |
| API `run`, pipelines a methodology runs | the call's acting identity | `authorize_graph_steps` over every step before the methodology starts; a denial is `unauthorized` and nothing runs |
| API `run_solution` | the call's acting identity | `authorize_graph_steps` over each stage graph before the solution Run is created |
| Governed console, simulated Python | the session key | `Node::run_graph` with the key (`acting_is_user` false) |

## The trusted local session

An empty acting identity is the trusted local session: the headed GUI, a console with no key,
and in-process callers (tests, the in-app test suite). Every other gate in the node already
lets that identity through without a check (`Node::authorize_object`, the registry gate), and
step authority does the same: its steps run unchecked and are not recorded per step. Over HTTP
and MCP the gate is always on (#799), so an API run always has a named principal.

## What this does not do yet

- **Host-injected runners are not covered.** The console's `set_graph_runner` (the churn
  engine, #580) and the GUI's churn path build their own `GraphExecutor` without a principal.
  They are local-session paths today. Study trials (`create_study`) also run their steps as
  the node: [#1322](https://github.com/oramasearch/zetta/issues/1322).
- **Per-step authority is not carried on the Run record.** It is resolved per step from the
  principal of the call that started the run; a run resumed or retried by the node itself runs
  as the local session ([#1322](https://github.com/oramasearch/zetta/issues/1322)).
- **Egress and filesystem.** Outbound network is the node-wide allowlist of
  [egress-policy](https://github.com/oramasearch/zetta/pull/1321) (CTL-4), not yet scoped per
  principal or per run; filesystem reads are not scoped per step:
  [#1323](https://github.com/oramasearch/zetta/issues/1323).
- **Approval before running.** An unapproved pipeline can still run through `agent_run`
  ([#913](https://github.com/oramasearch/zetta/issues/913)); step authority limits what such a
  run may do to what its caller may do, but it is not the approval gate.
