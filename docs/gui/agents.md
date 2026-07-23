# Agents view

The **Agents** section (Operations → Agents, `src/gui/app_agents.cpp`, #849) — the served
agents you can chat with. A thin, read-only viewer over the replicated `agent:<id>` records
(the same source the MCP/REST `list_agents` reads), plus an inline create affordance.

- **List + detail** via `master_detail`: each row is an agent (name, or the short id when
  unnamed; the model as meta). The detail pane shows the agent's model, id, created stamp,
  tools, and system prompt, with a **Delete agent** action.
- **New agent** (`draw_add_agent_form`, collapsed by default): name + model (required) +
  description + system prompt + comma-separated tools → a replicated `agent:<id>` via
  `agent::AgentStore` (a uuid + ISO timestamps stamped here), mirroring the `create_agent`
  capability. The outcome surfaces in the panel (`banner` — the engine's reason verbatim on
  failure).
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

**In flight:** the web-app equivalent (Agents section + chat + selector) — see
[#849](https://github.com/oramasearch/zetta/issues/849).
