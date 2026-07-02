# AX — agent experience

LLMs and programs driving the cluster. Agents are a first-class persona
(PER-4); AX is designed, not incidental.

- **MCP with full parity**
  ([../api/registry-design.md](../../api/registry-design.md)): every capability a
  schema-validated tool whose description is one agent-actionable sentence
  stating side effects; **prompts as progressive disclosure** (orientation →
  use-case runs → journeys → investigation → feedback) so an agent never
  needs the full tool table to start.
- **`llms.txt` progressive-disclosure endpoints**
  ([#645](https://github.com/oramasearch/zetta/issues/645)): the node serves
  its own agent-readable documentation over plain HTTP — `/llms.txt` (what
  this cluster is, the first three reads, where everything lives) →
  `/llms/<topic>.txt` → `/llms-full.txt` — generated from the docs + the live
  registry, embedding **live cluster facts** (node id, indexes, catalog
  entries). Custom beyond the community convention wherever AX demands it.
- **The spec index**
  ([#646](https://github.com/oramasearch/zetta/issues/646)): `/v1/specs` +
  `list_specs`/`get_spec` tools serving the OpenAPI document plus **every**
  JSON Schema in the system (capability schemas + the schema-as-code boundary
  set) as one navigable index of objects — discovery without reading source.
- **Observability threaded back**: every result carries
  trace/span/duration; `get_trace` shows intent · plan · performance · result
  shape including per-node spans of distributed work — an agent can always
  answer "what actually happened?"
  ([../architecture/observability.md](../../architecture/observability.md)).
- **Errors an agent can act on**: domain failures return the engine's
  path-qualified reason verbatim; consent challenges name exactly what an
  approval would permit — and can never be satisfied by the calling key
  itself.

Working checklists: the **api-dev** and **mcp-journeys** skills. Wave epic:
#614.
