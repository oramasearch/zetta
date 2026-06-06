# zetta docs

zetta is a database. A *zetta node* is one running instance; it runs **headed**
(default — with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers).
This directory documents the system; keep it in sync as the system changes.

- [architecture.md](architecture.md) — the node, run modes, data model, and module map.
- [storage.md](storage.md) — on-disk format (UUID folders, JSONL chunks) and the storage-engine seam.
- [query.md](query.md) — the dialect-agnostic query layer and the supported Postgres subset.
- [large-objects.md](large-objects.md) — storing files as large objects and importing JSON/JSONL records.
- [runtime.md](runtime.md) — the `zetta::runtime` async/coroutine seam over Capy/Corosio, and the migration plan.
- [cluster.md](cluster.md) — clustering equal peers: running multiple nodes, the connect command, the protocol.
- [replication.md](replication.md) — Automerge-CRDT replicated state, policies (replicate-to-all), and actor identity.
- [build.md](build.md) — building, running, and testing.

## Research / background

Long-form reference material (not descriptions of shipped code) lives in
[research/](research/):

- [research/coroutines.md](research/coroutines.md) — C++20/23 coroutine runtime
  design deep-dive (Boost.Asio/Cobalt, Seastar, folly::coro, stdexec) and the
  recommended path for zetta. Informs the planned coroutine adoption.
