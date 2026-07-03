# API docs

The programmatic control surface: the capability registry rendered as **MCP
tools** and (later) **REST routes** — drive the cluster without the GUI, with
the same governance as every other surface. Product framing:
[../product/control-surfaces.md](../product/control-surfaces.md); execution
plan: [../plans/mcp-service.md](../plans/mcp-service.md) (epic
[#614](https://github.com/oramasearch/zetta/issues/614)). Read here before any
MCP/HTTP work.

- [registry-design.md](registry-design.md) — **read first**: the settled design
  — one registry/many renderings, the main-thread bridge, auth = AccessKey +
  audit-on-authorize, long work = Run/ComputeJob handles (no new job infra),
  the error mapping, the consent challenge.
- [adding-a-capability.md](adding-a-capability.md) — the recipe + review
  self-check for every new tool/route.

Tool table, endpoints, and per-tool schemas land here with #615/#616/#617.
