# Indexing docs

Indexes over tables, behind the pluggable **`IndexBackend` seam** (build over partitions / query /
drop) with a per-node `IndexRegistry` that resolves an `Index` entity — or an IR `index` step — to a
backend (ENT-39 / ENT-40, ADR-34). The first real backend is BM25F full-text over the vendored
oramacore-cpp engine (guarded on the private submodule), and the distributed form follows the
per-partition compute-near-data topology: sub-index per owned partition, scatter-gather + merge/rank.
Vector / semantic / feature backends are additive behind the same seam.

- [index-backend.md](index-backend.md) — the `IndexBackend` seam + `IndexRegistry` + the in-memory Search reference backend + the replicated `Index` entity (ENT-39 / ENT-40, ADR-34).
- [oramacore-fulltext-index.md](oramacore-fulltext-index.md) — the first real backend: full-text BM25F over the native oramacore-cpp engine, `ZETTA_WITH_ORAMACORE`-guarded (#527).
- [index-step-executor.md](index-step-executor.md) — run-path wiring: an IR `index` step resolves a backend, builds a real queryable index, records the entity (`IndexStepExecutor`, #528).
- [distributed-fulltext.md](distributed-fulltext.md) — per-partition distributed full-text: sub-index per owned partition, scatter-gather + merge/rank matching a consolidated oracle (#529, TC-13).
