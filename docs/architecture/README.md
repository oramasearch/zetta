# Architecture docs

The system-level view of a zetta node: what a node is, its run modes (headed / headless), the data
model, and the interfaces everything else plugs into — the `zetta::runtime` async layer, the
schema-as-code JSON boundary discipline, and the true-hybrid cluster thesis. Read here first when
orienting on the codebase or deciding *where* a new capability belongs; the per-domain detail lives
in the sibling directories.

- [architecture.md](architecture.md) — the node, run modes, data model, the async runtime and cluster, pub/sub (node-local messaging, the one locking subsystem), and module map.
- [code-map.md](code-map.md) — the annotated file-by-file map of the codebase (build targets, include/zetta, src/core, src/gui, tests), including memory accounting, the Run-metric registry, and native trainer hooks.
- [../ops/memory.md](../ops/memory.md) — process RSS, allocator truth, and the per-subsystem ownership registry exposed by every node.
- [../pubsub/crud.md](../pubsub/crud.md) — node-local message mutation and async
  REST/MCP browse; pub/sub stores remain authoritative and D39 remains deferred.
- [../pubsub/dispatch.md](../pubsub/dispatch.md) — the node's shared pub/sub
  dispatch pool, topic-count sizing, ordering/fairness guarantees and settings.
- [observability.md](observability.md) — **a foundation**: every operation captures intent · plan · performance · result shape as OTel-style spans threaded by trace id across surfaces and nodes; crash reports; feedback bundles; the traced test suite.
- [coding-conventions.md](coding-conventions.md) — the canonical conventions reference: C++ style, the domain layout + namespace rule, boundaries/no-throw, interfaces, commit/branch flow.
- [owner-gate.md](owner-gate.md) — the Node owner-thread gate (#624): `run_on_owner` lets background workers drive engine runs while node writes land on the owner loop.
- [runtime.md](runtime.md) — `zetta::runtime` over Capy/Corosio: cross-thread events and fair IO scheduling (#1076), `parallel_for`, and the migration plan.
- [schemas.md](schemas.md) — **schema as code**: the embedded C++ draft-07 schema is the source of truth; `schemas/*.json` is generated + drift-checked (#464).
- [true-hybrid.md](true-hybrid.md) — one equal-peer cluster spanning heterogeneous hardware (CPU/GPU/ASIC, on-prem + cloud) and the interfaces that make it work, with a built-vs-planned matrix.
- [incremental-reconciliation.md](incremental-reconciliation.md) — changed-key replication reads, prefix-routed projection updates, full-resync fallbacks, and debounced CRDT persistence (#1079).
- [code-audit.md](code-audit.md) — living codebase health audit: convention drift, redundancies, smells, and reconciliation against the wtm blueprint rules (RULE-* / NFR-*).
