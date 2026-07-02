# DX — developer experience

People building zetta. DX is the repo teaching itself and the conventions
carrying the architecture, so any contributor (human or agent) lands changes
that look like the system.

- **Progressive disclosure of the repo**: root `CLAUDE.md` (rules + routing) →
  directory rule-cards → domain `README.md`s → pages; the annotated
  [code map](../../architecture/code-map.md) for navigation.
- **The carrying conventions**
  ([../architecture/coding-conventions.md](../../architecture/coding-conventions.md)):
  coroutines-first, observability-first, distributed-first, schema-as-code,
  no-throw boundaries, seams-not-special-cases, namespaces == directories.
- **Skills as reproducibility**: zetta-dev (the base checklist), gui-dev,
  api-dev, mcp-journeys, add-use-case — every recurring task is a checklist
  that points at its design doc.
- **The traced test suite as the flywheel**: every capability lands with a
  journey ([../testing/mcp-journeys.md](../../testing/mcp-journeys.md)); every
  testkit case runs in a trace (timing + result-shape history); anything an
  exploratory agent finds is promoted to a permanent regression.
- **Docs land in the same commit as the change** — the disclosure network is
  only useful while every level can be trusted.
