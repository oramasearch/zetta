# zetta plans

Living planning docs for in-flight efforts: **where we are, what we're trying to
accomplish, and how we'd like to try to do it.** Unlike the rest of `docs/` (which
describes the system *as built*), a plan weighs options and tracks a moving target —
so it names built-vs-next explicitly and links to the code + research it rests on.
When an effort ships, fold its durable outcomes into the system docs and trim the
plan.

- [cloud-provisioning.md](cloud-provisioning.md) — the **Add-nodes cloud burst**
  effort: standing up real nodes on a cloud the operator controls (detection, async
  provisioning, SKU fallback, editable placement — built on PR
  [#608](https://github.com/oramasearch/zetta/pull/608)) and the next slice that
  makes them **join the equal-peer cluster** (cloud-init + a routable/overlay seed).
- [mcp-service.md](mcp-service.md) — the **MCP + REST control surface**:
  one registry of governed, schema-validated tools over the existing engine
  entry points (read parity → action parity → REST/OpenAPI → simulated user
  tests → GUI activity surfacing), embedded in headed + headless nodes.
- [pubsub/](pubsub/README.md) — **pub/sub in zetta**, designed one question at a
  time and now built at stage 1: node-local topics use a shared dispatch pool sized
  by active topics; cluster-wide routing, replay and change events remain later
  stages. Decisions, research and open questions live in the folder.
- [virtual-tables.md](virtual-tables.md) — **virtual tables over CRDT entities**: a
  read-only table whose columns span three tiers (durable CRDT / ephemeral-synced /
  ephemeral-local), composed at query time so fast state (telemetry, `last_seen`)
  never churns the CRDT. Generalizes the `system.nodes` redesign (one table with
  fields, not a table per view); distinct from DDF and from a materialized view.
