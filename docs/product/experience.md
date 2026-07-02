# Experience — UX, DX, AX

Orama Zetta serves three audiences deliberately, and each has a designed,
documented experience. The shared spine: **one engine entry point per
capability, one governance model, one observability system** — every
experience is a rendering of the same registry and the same `system.*` read
model, so nothing any audience sees can drift from what the cluster actually
did.

## UX — user experience (humans operating the cluster)

The GUI is the **observability lens**: humans watch, comprehend, and approve;
driving is one click or one sentence, never form-filling
([control-surfaces.md](control-surfaces.md)).

- **Surfaces**: the headed desktop app (a cluster member — the spatial cluster
  view, live gauges, algorithm-at-work animation, graph canvas), and a **web
  interface** (planned: the same read model + registry rendered in a browser
  over REST/MCP — for the operator who isn't at the desktop and for sharing a
  cluster view across a team; no fourth behavior path, strictly a rendering).
- **Principles**: the view registry + widget grammar + one status language
  ([../gui/architecture.md](../gui/architecture.md)); design system discipline
  ([../gui/design-system.md](../gui/design-system.md)); empty states that
  teach; feedback that quotes the engine verbatim; consent gates that name
  exactly what would happen; every long operation visible as a job.
- **The loop**: watch what you (or your agent) asked for happen live; click
  through the object graph; export a feedback bundle when something is wrong
  ([../architecture/observability.md](../architecture/observability.md)).

## DX — developer experience (people building zetta)

- **Progressive disclosure of the repo itself**: CLAUDE.md rules → directory
  rule-cards → domain READMEs → pages; the code map; the skills (gui-dev,
  api-dev, mcp-journeys, add-use-case) that make every recurring task a
  checklist.
- **Conventions that carry the architecture**: coroutines-first,
  observability-first, distributed-first, schema-as-code, no-throw
  boundaries, seams-not-special-cases
  ([../architecture/coding-conventions.md](../architecture/coding-conventions.md)).
- **The traced test suite as the flywheel**: every capability lands with a
  journey; every testkit case runs in a trace; agent-discovered issues are
  promoted to permanent regressions
  ([../testing/mcp-journeys.md](../testing/mcp-journeys.md)).

## AX — agent experience (LLMs and programs driving the cluster)

Agents are a first-class persona (PER-4). AX is designed, not incidental:

- **MCP with full parity** ([../api/registry-design.md](../api/registry-design.md)):
  every capability a schema-validated tool with an agent-actionable
  description; **prompts as progressive disclosure** (orientation → use-case
  runs → journeys → investigation → feedback), so an agent never needs the
  full tool table to start.
- **`llms.txt` progressive-disclosure endpoints** (planned): the node serves
  its own agent-readable documentation over HTTP — `/llms.txt` (what this
  cluster is, the three reads to run first, where everything else lives)
  linking down to per-domain pages (`/llms/<topic>.txt`) and `/llms-full.txt`
  — generated from the same docs + registry, so a plain-HTTP agent (no MCP
  client) can orient itself with one GET. Custom beyond the community
  convention where AX demands it: the files embed live cluster facts (node
  id, indexes, catalog entries), not just static prose.
- **The spec index**: one endpoint (`/v1/specs`, mirrored as MCP tools) that
  serves **the OpenAPI document plus every JSON Schema in the system** —
  capability input/output schemas and all the schema-as-code boundary schemas
  (`schemas/*.json`) — as one navigable index of objects. An agent (or an
  integrator's codegen) can discover every shape the cluster speaks without
  reading source.
- **Observability threaded back**: every tool result carries
  trace/span/duration; `get_trace` shows intent · plan · performance · result
  shape, including per-node spans of distributed work — the agent can always
  answer "what actually happened?".
- **Errors an agent can act on**: domain failures return the engine's
  path-qualified reason verbatim; consent challenges name exactly what
  approval would permit.

## Serving — models and indexes as endpoints (planned)

Every trained **Model** and built **Index** is **cluster-internal by
default**, and can be **exposed as HTTP** through an ingress as a governed,
per-entity choice: `expose_model` / `expose_index` publish
`/v1/models/{name}/predict` and `/v1/indexes/{name}/search` (the distributed
scatter-gather path underneath), key-scoped and audited like every surface,
each request a trace span. Un-exposing is one action. This is the `serve`
step's full meaning: the pipeline doesn't just produce artifacts — it stands
them up as endpoints the customer's own systems call, without the data or the
model ever leaving the cluster.

## Where each is specified

| Experience | Design docs | Working checklists |
|---|---|---|
| UX | gui/architecture.md · gui/design-system.md · product/control-surfaces.md | gui-dev skill |
| DX | architecture/coding-conventions.md · architecture/observability.md · the rule-cards | zetta-dev · api-dev · add-use-case skills |
| AX | api/registry-design.md · api/adding-a-capability.md · this page | api-dev · mcp-journeys skills |
