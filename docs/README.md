# zetta docs

zetta is a database. A *zetta node* is one running instance; it runs **headed**
(default — with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers).
This directory documents the system; keep it in sync as the system changes.

- [architecture.md](architecture.md) — the node, run modes, data model, and module map.
- [projects.md](projects.md) — the AI project (`zetta::Project`) data model, lifecycle CRUD, and the execution graphs it owns.
- [project-inputs.md](project-inputs.md) — project inputs (`zetta::ProjectInput`): raw intent/artifact ingestion + provenance (replicated).
- [n8n-import.md](n8n-import.md) — importing an n8n workflow JSON as an artifact input (raw bytes + sha256 + schema-validated boundary).
- [runs.md](runs.md) — the Run (`zetta::Run`) record: status / logs / outputs / lineage, with JSON-Schema boundary validation.
- [access-control.md](access-control.md) — key-scoped access (V1): `Principal` + `AccessKey` + `Scope`, deny-by-default authorization (RULE-3).
- [storage.md](storage.md) — on-disk format (UUID folders, JSONL chunks) and the storage-engine seam.
- [query.md](query.md) — the dialect-agnostic query layer and the supported Postgres subset.
- [execution-graph.md](execution-graph.md) — the ExecutionGraph IR: typed step nodes/edges, the connect→clean→model→index→serve→act taxonomy, and its JSON form (`to_json`/`from_json` + schema validation).
- [deployments.md](deployments.md) — placing a validated execution graph onto a target (`DeploymentTarget`; R1 local), recorded + queryable.
- [deployment-controller.md](deployment-controller.md) — the control plane: reconcile a local deployment desired→actual and self-heal a killed/failed run (FEAT-4, TC-3; R1 local).
- [large-objects.md](large-objects.md) — storing files as large objects and importing JSON/JSONL records.
- [runtime.md](runtime.md) — the `zetta::runtime` async/coroutine seam over Capy/Corosio, and the migration plan.
- [cluster.md](cluster.md) — clustering equal peers: running multiple nodes, the connect command, the protocol.
- [replication.md](replication.md) — Automerge-CRDT replicated state, policies (replicate-to-all), and actor identity.
- [validation.md](validation.md) — the `ValidationReport` correctness-gate result type (per-check results, verdict, block-deploy).
- [gate.md](gate.md) — the correctness-gate runner (`zetta::CorrectnessGate`): register checks, run them over a graph, block deploy on failure (RULE-1).
- [gate-checks.md](gate-checks.md) — the concrete graph checks (`schema_type`, `data_leakage`): schema/type vs. source profiles + data-leakage / point-in-time for ML steps (#309).
- [build.md](build.md) — building, running, and testing.

## Research / background

Long-form reference material (not descriptions of shipped code) lives in
[research/](research/):

- [research/coroutines.md](research/coroutines.md) — C++20/23 coroutine runtime
  design deep-dive (Boost.Asio/Cobalt, Seastar, folly::coro, stdexec) and the
  recommended path for zetta. Informs the planned coroutine adoption.
