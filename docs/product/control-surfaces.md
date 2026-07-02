# Control surfaces — drive programmatically, watch visually

**The product decision:** Orama Zetta is driven through **programmatic control
surfaces first** — a rich, high-fidelity **MCP service** and its **HTTP REST**
sibling — and observed through the **headed GUI**. An agent (or a script, or a
power user) makes the cluster dance over MCP/REST without learning the UI; the
GUI's job is **superior observability** — the rich visual representation of
what the cluster was just asked to do and what it is doing about it.

This splits the classic control-plane UX into two roles that reinforce each
other instead of competing:

| Surface | Role | Who uses it |
|---|---|---|
| **MCP service** | Drive: every product action as a schema-validated tool; prompts as the methodology layer | Agents (Claude Code first), power users, our own test harnesses |
| **HTTP REST** | Drive: the same engine entry points for anything that speaks HTTP | Integrations, CI, customer systems |
| **SQL (`system.*`)** | Inspect: every observable state as a read-only table (RULE-7) | Everyone — the shared read model all surfaces render |
| **Headed GUI** | Watch: spatial cluster view, live gauges, algorithm-at-work animation, graph canvas, activity | Humans — observability and comprehension, not data entry |

## Why this is the right shape for this product

1. **The buyer's bottleneck is people-time, not pixels.** The one-pager's whole
   thesis is compressing intent → deployed intelligence. An agent driving MCP
   compresses that harder than any form ever will; the GUI earns its keep by
   making the result *legible* — the graph, the placement, the near-data vs
   wire flows, the drivers.
2. **It is how we validate the cluster while we build it.** With full tool
   parity, Claude Code can run **simulated user tests** against a live cluster:
   create a project, capture intent, import an n8n artifact, compile, gate,
   deploy, run UC-1 over real nodes, assert on `system.*` — the same journeys a
   design partner will run, executed continuously as test suites. Design flaws
   surface as awkward tool sequences long before a customer hits them.
3. **Agents are a first-class persona.** The blueprint already says so (PER-4
   "The Agent", API-6 MCP endpoint, FEAT-13 Personalized MCP); this page makes
   the ordering explicit: the MCP surface is not an add-on to the GUI — the GUI
   is the *lens* on a cluster that is primarily driven programmatically.
4. **Governance is already the hard part, and it is done once.** Every surface
   routes through the same seams: capability-scoped access keys (RULE-3),
   the correctness gate before anything runs (RULE-1), consent gates for
   anything that touches a cloud, and one audit trail. An MCP tool call is as
   governed as a GUI click — because they call the same engine entry points.

## The parity rule

**Every action the GUI can take is callable as an MCP tool, and every MCP tool
is visible in the GUI.** (The worktree-manager "full dashboard parity" rule,
adopted wholesale.) Concretely:

- A new product capability lands as an **engine entry point** (zetta-core)
  first; the MCP tool, REST route, and GUI affordance are all thin callers of
  it. No surface gets bespoke behavior.
- Observable state lands as a **`system.<table>` read view** (RULE-7); the GUI
  renders it, MCP/REST/SQL read it — one source, no drift.
- A **mutating tool call notifies the GUI live** (the embedded-server activity
  hook): when an agent deploys a graph or bursts nodes, the operator watching
  the headed app sees the graph animate and the node boxes light up *as it
  happens*. The GUI is the audience for the agent's performance.

## What this unlocks, in order

1. **Design + correctness validation now** — MCP-driven user journeys become
   runnable test suites over a real cluster (the testkit grows a remote driver);
   we use them to pressure-test the product's own ergonomics while the cluster
   is still being built.
2. **A rich user experience without UI learning** — a design partner points
   Claude (or curl) at their cluster and asks for outcomes; the GUI shows their
   team what happened, with lineage and placement made visual.
3. **Personalized MCP later (FEAT-13)** — once the generic surface exists, the
   per-user shaping (suggested use cases, preferred tools, learned defaults)
   layers on top of it, not beside it.

Execution plan + slices: [../plans/mcp-service.md](../plans/mcp-service.md).
Related blueprint artifacts: API-6 (MCP endpoint), API-8 (desktop control API),
API-7 (CLI), FEAT-10 (GUI control plane), FEAT-13 (Personalized MCP), PER-4
(The Agent), RULE-1/3/7. The reference implementation for the fidelity bar is
worktree-manager's `wtm-mcp` (Streamable HTTP, full parity, prompts as
methodology, embedded-in-GUI live refresh).
