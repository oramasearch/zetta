# Runbook — the agent builds + runs an execution graph from plain language

**Goal:** from a plain-language ask ("predict which customers are about to churn"), have the
base agent **recognize the use case, build a real `ir::ExecutionGraph`, and run it** on the
live engine — a trained model + served outputs, end to end, in one step.

This is [#872](https://github.com/oramasearch/zetta/issues/872). The base agent
([#555](https://github.com/oramasearch/zetta/issues/555)) is usable **right away**: it does
**not** need the RLVR tool-calling fine-tune ([#859](https://github.com/oramasearch/zetta/issues/859))
first. #859 is a later *enhancement* (the agent emitting the graph step by step); today the
engine compiles the graph from the same ask the agent recognizes.

## How it works (one capability)

`agent_run` composes three things you can also do by hand:

1. **Recognize** — the agent reads the ask and returns its solution (the `respond`/`chat` path).
2. **Build** — the engine compiles the same ask into a real, editable `ir::ExecutionGraph`
   (`pipeline::compile_intent` — the churn-shape heuristic), stored on a project.
3. **Run** — the graph goes through the correctness gate + the real engine (delegates to
   `run`; a churn graph trains a churn model over the live cluster).

```
ask ──▶ agent.respond ──▶ (recognizes churn)
    └──▶ compile_intent ──▶ ExecutionGraph {connect→clean→model→index→serve→act}
                        └──▶ gate ──▶ run ──▶ trained model + ranked at-risk list
```

## Prerequisites

- A **running node** (`./build/bin/zetta …`) with a bearer token that resolves to a user.
- A **served agent** whose `model` is a local base-agent checkpoint. Check with `list_agents`;
  create one in the **Agents** view (or `create_agent`) if there are none. Pull open weights
  first with `zetta pull <owner/model>` if the checkpoint isn't on disk
  (see [training-runbook.md](training-runbook.md)).

## Do it — three surfaces, one capability

### 1. MCP (Claude Desktop / Code, any MCP host) — the `build_and_run` prompt

The node ships an MCP **prompt** that guides the client through the whole flow. In an
MCP-connected chat, invoke the `build_and_run` prompt (hosts surface prompts as slash
commands / a prompt picker) with your `ask`; the client then calls `agent_run` and polls
`get_run` for you and reports the result.

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
# one call: recognize + build + run
curl -s -X POST $ENDPOINT/v1/agent_run \
  -H "Authorization: Bearer $ZETTA_TOKEN" -H 'Content-Type: application/json' \
  -d '{"agent":"<agent-id>",
       "message":"predict which customers are about to churn, with calibrated scores and explanations",
       "top_k":10}'
# -> { reply, shape:"churn", steps:[Load customers … Trigger retention outreach],
#      run:{ run_id, engine:"churn", status:"running" } }

# poll to completion
curl -s $ENDPOINT/v1/get_run -H "Authorization: Bearer $ZETTA_TOKEN" \
  -H 'Content-Type: application/json' -d '{"run_id":"<run_id>"}'
# -> status:"succeeded", outputs include a trained model + the served at-risk list
```

Optional inputs: `project` (land it on an existing project instead of a fresh one),
`graph_name`, `dataset_path` (a churn dataset dir; default is the built-in demo),
`model_name`, `max_new_tokens`.

### 3. GUI / web — the "Build & run" button

In the **Chat** view (headed app or the web UI), pick the agent, type the ask, and press
**Build & run** (beside Send). The agent's recognition + the built graph's steps post into
the transcript; a churn graph runs on the live engine and its **trained-model + ranked at-risk
list** posts back when it finishes. See [../gui/agents.md](../gui/agents.md).

## What you get

- **The agent's reply** — its recognition of the ask (the solution facts).
- **The built graph** — `shape` (e.g. `churn`) + `steps`
  (`connect → clean → model → index → serve → act`), a real editable graph on a project.
- **The run** — `run_id` + status; on success the `outputs` carry the trained **model**, the
  churn **index**, the served **at-risk list**, and the retention **playbook**. Inspect them
  in **Operations → Runs / Models / Jobs** or via `get_run` / `list_models` / `system.runs`.

## Verify (what "working" looks like)

The 0.6B base agent, on a churn ask, returns a clean single-block solution
(`{model_kind: supervised_binary_classifier, calibrated, leakage_free, point_in_time_features,
explanation, serve_ranked_list, …}`), the engine builds the 7-step graph, and `get_run`
reaches `succeeded` with a trained churn model — in a single ~18 s call.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no agents on a standalone node` | The node has no `ReplicationManager`. Use a normal (replicated) node. |
| `no agent '<id>'` | Wrong id — list with `list_agents` and pass the `id`. |
| `could not load the tokenizer/model from '<path>'` | The agent's `model` path isn't a checkpoint. Pull it (`zetta pull`) or fix the path. |
| `run is a job capability and this embedding wired no background action owner` | `agent_run`'s churn run needs the server's background owner — call it over MCP/REST/web (all wire it), not a bare embedding. |
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
