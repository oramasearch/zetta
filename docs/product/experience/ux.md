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
  a rendering/invocation of the existing capability surface, never a fourth behavior path. It wears
  the **Orama Studio shell** ([#874](https://github.com/oramasearch/zetta/issues/874),
  [design-sync.md](design-sync.md)): IBM Plex + Carbon Blue 60 tokens with a paired dark theme,
  and five sidebar groups — **Workspace** (Home stat tiles · Projects: index + rich detail with the
  lifecycle actions · Data library: a databases → tables → rows explorer plus a **Files** panel over
  `system.large_objects` · Models), **Operate** (Runs and activity: a run-vs-job legend, Pipelines,
  deployments, runs, jobs · Observability · Governance), **Developer** (Shell: a unified SQL/Python
  REPL with `/` commands · Notebooks · Agents · Chat · Data generators), **Documentation**
  (llms.txt · MCP · OpenAPI), and **Advanced** (Infrastructure — the cluster view, demoted per the
  design). User-facing copy follows the Studio vocabulary contract (Pipeline / Verification; raw
  graph terms only in the explicitly labeled Technical view). Stored names and
  Pipeline attributes remain text in the fallback page's embedded JSON (#1405);
  [script-context encoding](../../api/registry-design.md#json-embedded-in-html-1405)
  preserves their values without letting them become HTML. An installed Studio
  bundle receives data through the authenticated API instead.
  Login, logout and actions accept only this browser origin (#1415); a sibling
  site cannot use the session to submit them. Cookie API reads also need origin
  evidence, and SQL writes use POST. See the
  [browser request contract](../../api/registry-design.md#auth-governance-audit--nothing-new).
- **A coding agent (Claude Code first)** — for many users the primary way to
  operate zetta: they say what they want in their own words, the agent drives
  the MCP surface on their behalf, and the desktop/web lens shows them what
  their agent is doing as it happens (#620). Conversational operation is a
  designed UX mode, not a workaround — the human's words ride the `intent`
  field, so what they *meant* is recorded alongside what was invoked, and the
  trace + activity feed let them audit their agent afterwards.

With optional Clerk authentication configured, an issuer outage leaves native
token sign-in responsive (#1416). Existing Clerk keys can verify sessions for
up to 60 minutes after their last successful fetch; once that limit is reached,
Clerk requests return 401 until keys can be refreshed. The [authentication contract](../../governance/access-control.md#clerk-key-caching-1416)
describes the cache and rotation limits.

## Principles

- Structure: the view registry, the widget grammar, one status language,
  `navigate_to` + entity links ([../gui/architecture.md](../../gui/architecture.md)).
- Look + copy: the Orama design system ([../gui/design-system.md](../../gui/design-system.md))
  — sentence case, product language, the accent blue as "you are here". Engine enums
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
