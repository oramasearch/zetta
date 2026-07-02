# Architecture docs

The system-level view of a zetta node: what a node is, its run modes (headed / headless), the data
model, and the seams everything else plugs into — the `zetta::runtime` async layer, the
schema-as-code JSON boundary discipline, and the true-hybrid cluster thesis. Read here first when
orienting on the codebase or deciding *where* a new capability belongs; the per-domain detail lives
in the sibling directories.

- [architecture.md](architecture.md) — the node, run modes, data model, and module map.
- [code-map.md](code-map.md) — the annotated file-by-file map of the codebase (build targets, include/zetta, src/core, src/gui, tests).
- [runtime.md](runtime.md) — the `zetta::runtime` async/coroutine seam over Capy/Corosio (incl. `parallel_for`) and the migration plan.
- [schemas.md](schemas.md) — **schema as code**: the embedded C++ draft-07 schema is the source of truth; `schemas/*.json` is generated + drift-checked (#464).
- [true-hybrid.md](true-hybrid.md) — one equal-peer cluster spanning heterogeneous hardware (CPU/GPU/ASIC, on-prem + cloud) and the seams that make it work, with a built-vs-planned matrix.
- [code-audit.md](code-audit.md) — living codebase health audit: convention drift, redundancies, smells, and reconciliation against the wtm blueprint rules (RULE-* / NFR-*).
