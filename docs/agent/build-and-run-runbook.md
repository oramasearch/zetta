# Runbook — the agent builds + runs an execution graph from plain language

**Goal:** from a plain-language ask ("predict which customers are about to churn"), have the
base agent **recognize the use case and build a real `ir::ExecutionGraph`**, stored as a proposed
plan, then, **once a person approves that plan**, run it on the live engine: a trained model +
served outputs.

**`agent_run` never runs a plan nobody approved** ([#913](https://github.com/oramasearch/zetta/issues/913)).
It builds and stores the plan, and the call is refused with `not_approved`, naming the plan.
Approval is `approve_plan`, by a principal who holds `orchestration:approve_plan`; `agent_run`
needs only `orchestration:run` and never approves. The same `agent_run` with `project` and
`graph_name` naming the approved plan then runs it. Before #913, `agent_run` recorded the
caller's approval of the plan it had just built and ran it in the same call, so a model was
trained and an `act` step fired on a Pipeline no person had reviewed.

This is [#872](https://github.com/oramasearch/zetta/issues/872). The base agent
([#555](https://github.com/oramasearch/zetta/issues/555)) is usable **right away**: it does
**not** need the RLVR tool-calling fine-tune ([#859](https://github.com/oramasearch/zetta/issues/859))
first. #859 is a later *enhancement* (the agent emitting the graph step by step); today the
engine compiles the graph from the same ask the agent recognizes.

## How it works (build, approve, run)

1. **Recognize** — the agent reads the ask and returns its solution (the `respond`/`chat` path).
2. **Build** — the engine compiles the same ask into a real, editable `ir::ExecutionGraph`
   (`pipeline::compile_intent` — the churn-shape heuristic), stored on a project as a
   **proposed** plan. Nothing runs: the call answers `not_approved` with the project and plan
   name.
3. **Approve** — a person reviews the plan (`get_graph`, or the Studio's proposed-plan view)
   and approves it with `approve_plan { project, graph }`. The approval records who and when,
   and is audited (`graph_approved`).
4. **Run** — `agent_run` with the same `project` and `graph_name` finds the stored plan, checks
   it is approved, and runs it through the correctness gate + the real engine (delegates to
   `run`; a churn graph trains a churn model over the live cluster). A plan still proposed is
   refused with `not_approved`, before the agent's model loads. Editing an approved plan
   re-proposes it, so it must be approved again.

```
ask ──▶ agent.respond ──▶ (recognizes churn)
    └──▶ compile_intent ──▶ ExecutionGraph {connect→clean→model→index→serve→act}, proposed
                        └──▶ not_approved (nothing ran)
approve_plan ──▶ approved
agent_run {project, graph_name} ──▶ correctness gate ──▶ run ──▶ trained model + ranked at-risk list
```

## Prerequisites

- A **running node** (`./build/bin/zetta …`) with a bearer token that resolves to a user.
- A **served agent** whose `model` selects an [operator-registered checkpoint](checkpoints.md)
  (#1411), such as `base-agent`. Check with `list_agents`;
  create one in the **Agents** view (or `create_agent`) if there are none. Pull open weights
  first with `zetta pull <owner/model>` if the checkpoint isn't on disk
  (see [training-runbook.md](training-runbook.md)).

## Do it — three surfaces, one capability

### 1. MCP (Claude Desktop / Code, any MCP host) — the `build_and_run` prompt

The node ships an MCP **prompt** that guides the client through the whole flow. In an
MCP-connected chat, invoke the `build_and_run` prompt (hosts surface prompts as slash
commands / a prompt picker) with your `ask`; the client then calls `agent_run`, shows you the
plan it built, approves it only when you say yes, runs it, polls `get_run` for you and reports
the result.

Raw JSON-RPC (what the host does under the hood):

```sh
# discover the prompt
curl -s $ENDPOINT/mcp -H "Authorization: Bearer $ZETTA_TOKEN" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"prompts/list"}'

# expand it with your ask -> a ready-to-run instruction turn
curl -s $ENDPOINT/mcp -H "Authorization: Bearer $ZETTA_TOKEN" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"prompts/get","params":{"name":"build_and_run",
       "arguments":{"ask":"predict which enterprise accounts will churn next quarter"}}}'
```

`agent_run` is also a plain MCP **tool** (`tools/call`), so a host can skip the prompt and
call it directly.

### 2. REST / curl

```sh
# 1. recognize + build: the plan is stored proposed and nothing runs
curl -s -X POST $ENDPOINT/v1/agent_run \
  -H "Authorization: Bearer $ZETTA_TOKEN" -H 'Content-Type: application/json' \
  -d '{"agent":"<agent-id>",
       "message":"predict which customers are about to churn, with calibrated scores and explanations",
       "project":"Churn","graph_name":"churn plan","top_k":10}'
# -> error not_approved: "agent_run built the plan 'churn plan' on project 'Churn' (7 steps,
#    shape churn) and stored it proposed; nothing ran. Approve it (approve_plan), then …"

# 2. review, then approve (needs orchestration:approve_plan)
curl -s -X POST $ENDPOINT/v1/approve_plan \
  -H "Authorization: Bearer $ZETTA_TOKEN" -H 'Content-Type: application/json' \
  -d '{"project":"Churn","graph":"churn plan"}'

# 3. run the approved plan
curl -s -X POST $ENDPOINT/v1/agent_run \
  -H "Authorization: Bearer $ZETTA_TOKEN" -H 'Content-Type: application/json' \
  -d '{"agent":"<agent-id>","message":"run the churn plan",
       "project":"Churn","graph_name":"churn plan","top_k":10}'
# -> { reply:"Running the plan 'churn plan', approved by …", shape:"approved",
#      steps:[Load customers … Trigger retention outreach],
#      run:{ run_id, engine:"churn", status:"running" } }

# poll to completion
curl -s $ENDPOINT/v1/get_run -H "Authorization: Bearer $ZETTA_TOKEN" \
  -H 'Content-Type: application/json' -d '{"run_id":"<run_id>"}'
# -> status:"succeeded", outputs include a trained model + the served at-risk list
```

Optional inputs: `project` (land it on an existing project instead of a fresh one; without it
the refusal names the fresh project it created), `graph_name` (with `project`, a plan already
stored there is run if approved and refused if proposed), `dataset_path` (a churn dataset dir;
default is the built-in demo), `model_name`, `max_new_tokens`.

### 3. GUI / web — the "Build plan" and "Build & run" buttons

In the web UI's **Chat** view, pick the agent, type the ask, and press **Build plan** (beside
Send). It calls `agent_run`, so the plan is stored proposed and the page shows the
`not_approved` answer naming it; approve it with the `approve_plan` form, then run it with the
`agent_run` form (project + graph_name).

The headed app's **Chat** view keeps **Build & run**, which does not call `agent_run`: it runs
the compiled graph directly in the trusted local session (the person at the window). That path
is tracked in [#1334](https://github.com/oramasearch/zetta/issues/1334) (see [../gui/agents.md](../gui/agents.md)).

## What you get

- **The agent's reply** — its recognition of the ask (the solution facts).
- **The built graph** — a proposed plan on a project, named in the `not_approved` answer:
  `connect → clean → model → index → serve → act`, real and editable.
- **After approval, the run's answer** — `shape:"approved"` + `steps`, and the reply names who
  approved the plan and when.
- **The run** — `run_id` + status; on success the `outputs` carry the trained **model**, the
  churn **index**, the served **at-risk list**, and the retention **playbook**. Inspect them
  in **Operations → Runs / Models / Jobs** or via `get_run` / `list_models` / `system.runs`.

## Verify (what "working" looks like)

The 0.6B base agent, on a churn ask, returns a clean single-block solution
(`{model_kind: supervised_binary_classifier, calibrated, leakage_free, point_in_time_features,
explanation, serve_ranked_list, …}`), the engine builds the 7-step graph, and `get_run`
reaches `succeeded` with a trained churn model once the plan is approved and `agent_run` runs
it.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no agents on a standalone node` | The node has no `ReplicationManager`. Use a normal (replicated) node. |
| `no agent '<id>'` | Wrong id — list with `list_agents` and pass the `id`. |
| `agent.model: checkpoint is not registered` | Ask the node operator to register the model selector; a download alone does not authorize serving. |
| `could not load the tokenizer/model from '<path>'` | The registered directory lacks a usable checkpoint. The operator must install the expected files. |
| `run is a job capability and this embedding wired no background action owner` | `agent_run`'s churn run needs the server's background owner — call it over MCP/REST/web (all wire it), not a bare embedding. |
| `not_approved: agent_run built the plan …` | Expected: the plan is stored proposed and nothing ran. Review it, `approve_plan`, then call `agent_run` again with `project` + `graph_name`. |
| `not_approved: the plan '…' … is proposed` | The named plan was never approved, or was edited since (an edit re-proposes). Approve it with `approve_plan`. |
| `blocked by gate: …` | The compiled graph failed a correctness check; the reason names the failing check. |
| `shape:"generic"` (not churn) | The ask didn't match the churn heuristic; the generic graph runs **structurally** (no trained model). Rephrase toward the use case, or grow the compiler's use-case coverage. |

## Related

- [inference-runbook.md](inference-runbook.md) — run the agent + grade its solution; Step 4 is
  the `agent_run` summary this runbook expands.
- [training-runbook.md](training-runbook.md) · [training-data-runbook.md](training-data-runbook.md)
  — produce the checkpoint + corpus the agent is built from.
- [../gui/agents.md](../gui/agents.md) — the Agents + Chat views + the Build & run button.
- The MCP prompt lives in `src/core/api/mcp_prompts.cpp`; `agent_run` in
  `src/core/api/capabilities.cpp`.
- [#872](https://github.com/oramasearch/zetta/issues/872) (this flow) ·
  [#555](https://github.com/oramasearch/zetta/issues/555) (base agent) ·
  [#849](https://github.com/oramasearch/zetta/issues/849) (chat) ·
  [#859](https://github.com/oramasearch/zetta/issues/859) (optional RLVR trajectory agent).
