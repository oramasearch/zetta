# Agents view

The **Agents** section (Operations → Agents, `src/gui/app_agents.cpp`, #849) — the served
agents you can chat with. A thin viewer over the replicated `agent:<id>` records
(the same source the MCP/REST `list_agents` reads), plus inline create + edit affordances.

- **List + detail** via `master_detail`: each row is an agent (name, or the short id when
  unnamed; the model as meta). The detail pane shows the agent's model, id, created stamp,
  tools, and system prompt, with **Open chat**, **Edit**, load/unload, and **Delete agent**
  actions.
- **New agent** (`draw_add_agent_form`, collapsed by default): name + model (required) +
  description + system prompt + comma-separated tools → a replicated `agent:<id>` via
  `agent::AgentStore` (a uuid + ISO timestamps stamped here), mirroring the `create_agent`
  capability. The outcome surfaces in the panel (`banner` — the engine's reason verbatim on
  failure).
- **Edit an existing agent** (`draw_edit_agent_form`): **Edit** in the detail pane seeds a
  working copy (`edit_agent_*`) from the record and swaps the pane to an editable form —
  **name, model, description, system prompt, tools**. **Save changes** writes it back through
  `AgentStore::put` (preserving id + created_at, bumping updated_at), mirroring the
  `update_agent` capability. The model isn't reloaded on a prompt change — `respond` applies
  the system prompt per turn, so the edit takes effect on the very next chat message.
- **Live source**: agents are re-read from `AgentStore(node.replication()).list()` each
  frame; no panel-owned copy. A standalone (unreplicated) node shows the empty state.

Model to point an agent at: a local checkpoint dir (pull open weights with `zetta pull`), a
`model:<uuid>`, or a HuggingFace repo id.

## Chat view

The **Chat** section (Operations → Chat, `src/gui/app_chat.cpp`) — talk to a selected agent
with tool-calling. An **agent selector** (combo) picks which agent to talk to; the transcript
scrolls above a pinned input row. A message runs the agent's `chat` turn (model load +
generation + the tool-calling loop, via `agent::respond` — the same path the MCP/REST `chat`
capability uses) **off the UI thread** through a `ChatOp` (a `std::thread` + shared done flag,
applied on the UI thread by `poll_chat_op()` in `poll_background_ops()`, the #624 pattern), so
a ~13 s turn never freezes the frame. The reply shows with a one-line tool summary
(`connect(ok) · serve(ok)`); a load failure surfaces as the panel's error `banner`.

### Build & run (#872)

Beside **Send**, a **Build & run** button makes the base agent do real work — no #859
tool-calling fine-tune needed. The same off-thread `ChatOp` runs `respond` **and**
`pipeline::compile_intent(message)`; `poll_chat_op()` then summarizes the compiled graph's
steps into the transcript and **runs it** — a churn graph launches `AsyncChurnRun` on the live
engine (its trained-model + ranked-list outcome posts back into the transcript via the
`churn_run_from_chat_` branch of `poll_background_ops()`), anything else runs structurally.
It is the GUI counterpart of the `agent_run` capability (`docs/agent/inference-runbook.md`), but it
does not call it: it runs the compiled graph directly in the trusted local session, without the
approval `agent_run` requires since #913. That difference is tracked in
[#1334](https://github.com/oramasearch/zetta/issues/1334).

## Web app

The server-rendered web UI (`src/core/api/http_server.cpp`, #647) has the same two views in
its nav (Workloads → **Agents** / **Chat**): the Agents page lists the records — each row
carries an inline, **pre-filled `update_agent` form** (`<details>` → name / description /
**system prompt** / tools, the tools field JSON-encoded so `coerce_query_value` parses it),
so you edit an existing agent's prompt in place without retyping; empty fields aren't sent, so
a partial edit changes only what you touch. The generic create/update/delete_agent action
forms sit below. The **Chat** page is a form with an **agent `<select>`**
(the `agent`-field picker → `agent_options`, from the live agent list) + a message box that
POSTs to `/run` and renders the result inline. The Chat page carries **two** submit buttons —
**Send** (`cap=chat`, reply only) and **Build plan** (`cap=agent_run`, #872) — the clicked
button's `name=cap` picks the capability. **Build plan** stores the compiled plan proposed and
renders the `not_approved` answer that names it; nothing runs until it is approved
(`approve_plan`) and run (`agent_run` with project + graph_name, both from the generic action
forms; #913). `serve_web_run` already wires the background action owner, so the approved churn
run works with no extra plumbing. The turn blocks the request for seconds (model load +
generation) — fine for a request/response page (the GUI's off-thread `ChatOp` is what a
live-repainting client needs). Same `agent::respond` + `agent_run` paths as MCP/REST.
