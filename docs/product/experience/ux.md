# UX — user experience

Humans operating the cluster: they **watch, comprehend, and approve**; driving
is one click or one sentence, never form-filling. The GUI is the observability
lens on a cluster that is primarily driven programmatically
([control-surfaces.md](../control-surfaces.md)).

## Surfaces

- **The headed desktop app** — itself a cluster member: the spatial cluster
  view with live gauges, the algorithm-at-work animation, the graph canvas,
  master-detail panels over the `system.*` read model.
- **The web interface** ([#647](https://github.com/oramasearch/zetta/issues/647), served by
  the node's own `:4800` HTTP server, `src/core/api/web_ui.cpp`) — the same read model +
  registry rendered **server-side as straight HTML** in a browser, for operators away from the
  desktop and for sharing a cluster view across a team. Cookie-authed (`zetta_session`); strictly
  a rendering/invocation of the existing capability surface, never a fourth behavior path. Nav:
  **Overview** (clickable stat tiles), **Cluster**, **Projects** (index + rich detail: overview,
  inputs, graphs, runs, models + the lifecycle actions), **Data** (a databases → tables → rows
  explorer plus a **Files** panel over `system.large_objects` — upload/download/delete), **Models**,
  **Pipeline** (a run-vs-job legend, graphs, deployments, runs, jobs), **Observability**,
  **Governance**, **Shell** (a unified SQL/Python REPL with `/` commands), and **Notebooks**.
- **A coding agent (Claude Code first)** — for many users the primary way to
  operate zetta: they say what they want in their own words, the agent drives
  the MCP surface on their behalf, and the desktop/web lens shows them what
  their agent is doing as it happens (#620). Conversational operation is a
  designed UX mode, not a workaround — the human's words ride the `intent`
  field, so what they *meant* is recorded alongside what was invoked, and the
  trace + activity feed let them audit their agent afterwards.

## Principles

- Structure: the view registry, the widget grammar, one status language,
  `navigate_to` + entity links ([../gui/architecture.md](../../gui/architecture.md)).
- Look + copy: the Orama design system ([../gui/design-system.md](../../gui/design-system.md))
  — sentence case, product language, coral as "you are here". Engine enums
  render as product words (`label_of`, one mapping site); the raw wire values
  stay in SQL and the API ([../gui/copy-audit.md](../../gui/copy-audit.md)).
- Empty states teach (one sentence + the single next action); feedback quotes
  the engine verbatim; consent gates name exactly what would happen; long
  operations are visible jobs, never frozen frames (#624 —
  [../gui/long-running-actions.md](../../gui/long-running-actions.md)).
- Keyboard-first for operators who live in the app: the Cmd+K command palette
  ([../gui/command-palette.md](../../gui/command-palette.md)) reaches every
  view, every named entity, and the capability registry's actions — the tool
  list rendered for humans, same gating everywhere (#625).

## The loop

Landing on Home answers the operator's first questions before a single click —
is the cluster healthy, what ran, what needs attention, how fast intent became
a deployment ([../gui/home-dashboard.md](../../gui/home-dashboard.md), SCR-1).
Watch what you (or your agent) asked for happen live; click through the object
graph (run → model → job → graph); export a feedback bundle when something is
wrong ([../architecture/observability.md](../../architecture/observability.md)).

Working checklist: the **gui-dev** skill. Wave epic: #627.
