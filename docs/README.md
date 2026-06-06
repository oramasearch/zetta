# zetta docs

zetta is a database. A *zetta node* is one running instance; it runs **headed**
(default — with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers).
This directory documents the system; keep it in sync as the system changes.

- [architecture.md](architecture.md) — the node, run modes, data model, and module map.
- [storage.md](storage.md) — on-disk format (UUID folders, JSONL chunks) and the storage-engine seam.
- [query.md](query.md) — the dialect-agnostic query layer and the supported Postgres subset.
- [build.md](build.md) — building, running, and testing.
