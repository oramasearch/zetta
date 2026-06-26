# zetta docs

zetta is a database. A *zetta node* is one running instance; it runs **headed**
(default — with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers).
This directory documents the system; keep it in sync as the system changes.

- [architecture.md](architecture.md) — the node, run modes, data model, and module map.
- [true-hybrid.md](true-hybrid.md) — **true hybrid**: one equal-peer cluster spanning heterogeneous hardware (CPU / GPU / ASIC, OS, architecture, on-prem + cloud) and the seams that make it work — `ComputeDevice` backends, the device registry, the heterogeneity-tolerant work-stealing scheduler, collectives, the equal-peer CRDT cluster, and the `Provisioner` seam — with a built-vs-planned support matrix.
- [projects.md](projects.md) — the AI project (`zetta::Project`) data model, lifecycle CRUD, and the execution graphs it owns.
- [project-inputs.md](project-inputs.md) — project inputs (`zetta::ProjectInput`): raw intent/artifact ingestion + provenance (replicated).
- [n8n-import.md](n8n-import.md) — importing an n8n workflow JSON as an artifact input (raw bytes + sha256 + schema-validated boundary).
- [n8n-compile.md](n8n-compile.md) — compiling an imported n8n workflow JSON into the ExecutionGraph IR (nodes → typed steps, connections → edges; FEAT-2 / #308).
- [new-project-screen.md](new-project-screen.md) — the headed **New Project** screen (SCR-2): name a project, paste an NL intent, and import / drop an n8n workflow artifact — all from the GUI (FEAT-1).
- [runs.md](runs.md) — the Run (`zetta::Run`) record: status / logs / outputs / lineage, with JSON-Schema boundary validation.
- [models.md](models.md) — the Model (`zetta::Model`) entity (ENT-14 / FEAT-6): metadata replicated as `model:<uuid>`, serialized weights as a large-object body (peer-to-peer + sha256), the `system.zetta_models` registry, JSON-Schema validated.
- [compute-jobs.md](compute-jobs.md) — the ComputeJob (`zetta::ComputeJob`) record: a unit of native distributed accelerated compute (featurize/train/explain/score/ddf), lifecycle + audit + JSON-Schema boundary (ARC-11 / ENT-31).
- [access-control.md](access-control.md) — key-scoped access (V1): `Principal` + `AccessKey` + `Scope` (resources) + `Capability` (rights, TTL'd), deny-by-default authorization (RULE-3 / ENT-22).
- [audit-trail.md](audit-trail.md) — the immutable, append-only `AuditEvent` (ENT-23 / FEAT-9): graph change / deploy / data-access events, replicated + JSON-Schema validated.
- [admin-panel.md](admin-panel.md) — the headed **Admin & Settings panel** (`App::draw_admin`, SCR-7 / CMP-5): read-only view of principals + keys, scope + capability grants, and the audit log.
- [storage.md](storage.md) — on-disk format (UUID folders, JSONL chunks) and the storage-engine seam.
- [query.md](query.md) — the dialect-agnostic query layer and the supported Postgres subset.
- [execution-graph.md](execution-graph.md) — the ExecutionGraph IR: typed step nodes/edges, the connect→clean→model→index→serve→act taxonomy, and its JSON form (`to_json`/`from_json` + schema validation).
- [intent-compiler.md](intent-compiler.md) — compiling a natural-language intent into a reviewable, editable ExecutionGraph skeleton (`zetta::compile_intent`; templated churn mapping, FEAT-2 / #311).
- [deployments.md](deployments.md) — placing a validated execution graph onto a target (`DeploymentTarget`; R1 local), recorded + queryable.
- [deployment-controller.md](deployment-controller.md) — the control plane: reconcile a local deployment desired→actual and self-heal a killed/failed run (FEAT-4, TC-3; R1 local).
- [execution.md](execution.md) — executing an ExecutionGraph on a node (`zetta::GraphExecutor`): walk steps in dependency order, drive a Run to Succeeded/Failed.
- [solution-catalog.md](solution-catalog.md) — the AI Solution Library (`zetta::SolutionCatalog`) and the headed **Deploy panel**: pick a blueprint, choose a placement on the dial, gate, and deploy (FEAT-4 / SCR-5 / CMP-3).
- [provisioner.md](provisioner.md) — the `Provisioner` seam + node capability model (`zetta::NodeCapability`): provision N nodes against a capability spec / tear down / list; cores / GPU type+throughput / memory (ARC-13 / ENT-32, #373) — plus the `LocalProvisioner` given-hardware backend (uses existing cluster nodes, best-effort; teardown is a no-op; #374).
- [burst-identity.md](burst-identity.md) — bulk-mint a short-lived service `Principal` + `AccessKey` per burst node on provision and bulk-revoke on teardown (`zetta::BurstIdentityManager`), reusing the identity model, audited (ARC-13 / RSK-9, #375).
- [scaling-policy.md](scaling-policy.md) — the job objective / reward model (`zetta::ScalingPolicy`): a per-job autoscaler objective (minimize cost / runtime-latency target / data shape) with bounds (min/max nodes, cost cap) + objective evaluation over (progress, elapsed, cost) samples (ADR-29 / ENT-34, #370).
- [burst-approval-gate.md](burst-approval-gate.md) — the large-burst cost-cap approval gate (`zetta::BurstApprovalGate`): a `Provisioner` decorator that blocks an over-cap provision burst pending approval and records the decision in the audit trail (APR-3 / RSK-9, #379).
- [large-objects.md](large-objects.md) — storing files as large objects and importing JSON/JSONL records.
- [compute-device.md](compute-device.md) — the `zetta::compute::ComputeDevice` operator interface + typed device-buffer / memory model + the host-memory stub (ADR-27 / ARC-9, accelerated compute; #359), plus the `zetta-compute` build target + the optional CUDA backend (`CudaDevice`; #363).
- [cpu-device.md](cpu-device.md) — the `zetta::compute::CpuDevice` reference backend: every operator on host memory, multi-threaded via `runtime::parallel_for` (ADR-27 / ARC-9; #360).
- [device-registry.md](device-registry.md) — the per-node device registry (`zetta::compute::DeviceRegistry`): enumerate a node's compute devices, report each one's capability, and select a backend per op (CPU-first / GPU-when-present) (ARC-9, #362).
- [work-stealing-scheduler.md](work-stealing-scheduler.md) — the work-stealing scheduler (`zetta::compute::WorkStealingScheduler` local + `DistributedScheduler` cross-node): a job split into tasks, idle/faster workers/nodes steal pending work (ARC-10; #366).
- [runtime.md](runtime.md) — the `zetta::runtime` async/coroutine seam over Capy/Corosio (incl. the `parallel_for` data-parallel primitive), and the migration plan.
- [cluster.md](cluster.md) — clustering equal peers: running multiple nodes, the connect command, the protocol.
- [signal-plane.md](signal-plane.md) — the ephemeral signal plane (`zetta::Signal`): RPC / events / tasks over the cluster transport, addressed by node id, schema-validated at the boundary (ADR-28 / ARC-10).
- [collectives.md](collectives.md) — collective communication (`zetta::Collectives`): allreduce / broadcast / barrier / gather over the signal plane, deterministic across node order, for combining compute partials (ADR-28 / ARC-10; #365).
- [replication.md](replication.md) — Automerge-CRDT replicated state, policies (replicate-to-all), and actor identity.
- [validation.md](validation.md) — the `ValidationReport` correctness-gate result type (per-check results, verdict, block-deploy).
- [gate.md](gate.md) — the correctness-gate runner (`zetta::CorrectnessGate`): register checks, run them over a graph, block deploy on failure (RULE-1).
- [gate-checks.md](gate-checks.md) — the concrete graph checks (`schema_type`, `data_leakage`): schema/type vs. source profiles + data-leakage / point-in-time for ML steps (#309).
- [entity-resolution-check.md](entity-resolution-check.md) — the entity-resolution correctness eval (`zetta::EntityResolutionCheck`, TC-4): fails the gate when a graph conflates separate accounts (the FedEx-Freight case).
- [churn-dataset.md](churn-dataset.md) — the synthetic UC-1 churn dataset fixture + loader (`zetta::ChurnDataset` / `load_churn_dataset`, FEAT-6 / #402): committed accounts + events, the FedEx entity-resolution case, schema-validated, no foreign runtime.
- [build.md](build.md) — building, running, and testing.

## Research / background

Long-form reference material (not descriptions of shipped code) lives in
[research/](research/):

- [research/coroutines.md](research/coroutines.md) — C++20/23 coroutine runtime
  design deep-dive (Boost.Asio/Cobalt, Seastar, folly::coro, stdexec) and the
  recommended path for zetta. Informs the planned coroutine adoption.
