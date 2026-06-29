# In-app test suite (`zetta::testkit`)

A headless-runnable catalog of **canned, parameterized test cases** that exercise
zetta's real primitives end to end — compute / device operators, DDF ops, the ML
pipeline, distributed / cluster behavior over a **real multi-node loopback cluster**,
governance (access + audit + the correctness gate), and the whole UC-1 churn
pipeline — plus the headed **Test suite** panel that runs them interactively (issue
#462). Built on the [UC-1 end-to-end](uc1-end-to-end.md) (#399).

```
include/zetta/testkit/test_suite.hpp   TestCase / TestOption / TestOptions /
                                       TestResult / TestSuite + run_test_case
include/zetta/testkit/harness.hpp      ScratchDir, LoopbackCluster, the in-code
                                       synthetic churn dataset + numeric encoding
src/core/testkit/test_suite.cpp        the engine (options, result, runner, builtin)
src/core/testkit/harness.cpp           the loopback cluster + dataset helpers
src/core/testkit/cases_*.cpp           the canned cases, one file per category
src/gui/app.cpp (draw_test_suite)      the headed renderer
tests/test_testkit.cpp                 runs every representative case headless
```

## The model — catalog in core, GUI as renderer

The catalog lives in **zetta-core**, so it is runnable headless and carries no SDL
dependency; the GUI is a thin renderer over it (the same rule SDL/ImGui already
follow). A `TestCase` is:

- an `id` (stable, dotted — e.g. `compute.cpu.elementwise`), a `TestCategory`, a
  name + description;
- typed **`TestOption`s** — the per-test "options modal" (bool / int / double /
  choice / text, with defaults + bounds);
- a `run(TestOptions, TestContext) -> TestResult` body that calls the real engine.

`TestSuite::builtin()` assembles the catalog once. `run_test_case` times the body and
**converts any thrown exception into a clean `Error` result**, so a misbehaving case
can never take the app down. A `TestResult` carries an outcome (`Pass` / `Fail` /
`Skipped` / `Error`), a one-line summary, key/value **metrics**, a step **log**, and
the elapsed time. A `TestContext` carries an optional **custom dataset path** (a churn
dataset directory the data-driven cases load instead of the built-in synthetic one)
and a progress-log sink.

## The cases

| Category | Cases | What they check |
|----------|-------|-----------------|
| **Compute & device** | `compute.cpu.elementwise`, `compute.cpu.reduce_sort`, `compute.cuda.conformance` | CpuDevice operators (map / elementwise / reduce / sort_by_key) against the host; the CUDA build state (skipped on a CPU-only build) |
| **DDF** | `ddf.scan_filter`, `ddf.group_by`, `ddf.parquet_reader_parity` | scan a table into a typed columnar frame, filter via the device gather op, and group-by aggregate — checked against a host computation; the native (no-Arrow) [Parquet reader](parquet-reader.md) loads a projected subset of a pyarrow-produced file into a DataFrame and matches the embedded pyarrow golden on schema / row count / dtypes / values, dropping the `embedding_*` columns (TC-9 / #499) |
| **Machine learning** | `ml.gbdt_train`, `ml.tree_shap`, `ml.calibration` | train a histogram GBDT (AUC); exact TreeSHAP (the efficiency identity `base + Σφ = raw` + the dominant driver); probability calibration (Platt / isotonic) |
| **Distributed & cluster** | `cluster.membership`, `cluster.allreduce`, `cluster.group_by_consistency`, `cluster.compute_near_data`, `cluster.partition_rebalance` | a real N-node loopback cluster forms; an allreduce returns the identical global result on every node; a distributed group-by equals the single-node computation; a [compute-near-data](compute-near-data.md) group-by runs over each node's locally-owned partitions, moving only the partials over the wire (≪ the dataset read near data) while matching the single-node oracle (TC-11 / #501); and a partitioned table rebalances on a node join — the rendezvous re-placement reflects in `system.dataset_partitions` on every node, `rf` preserved, every row still readable (TC-10 / [#500](https://github.com/oramasearch/zetta/issues/500)) |
| **Governance** | `governance.access_control`, `governance.audit`, `governance.entity_resolution_gate`, `governance.burst_identity` | deny-by-default key-scoped access; the immutable audit trail; the entity-resolution gate (FullName passes, Brand conflates the FedEx siblings and is blocked — TC-4 / RULE-1); bursting N nodes through the real cloud-burst provisioner mints N distinct short-lived identities and teardown revokes them with no orphans (TC-6 / ADR-20 / RSK-9) |
| **UC-1 pipeline** | `pipeline.uc1_end_to_end` | the whole gated + governed churn pipeline (the [#399 end-to-end](uc1-end-to-end.md)) as one runnable case |
| **Example scripts** | `examples.churn_pipeline`, `examples.minimal_train`, `examples.method_chain`, `examples.unterminated_string`, `examples.no_pipeline_steps` | sim-Python example programs run end to end (parse → IR → gate → run) and asserted on — *the example programs ARE the integration tests* ([example-scripts.md](example-scripts.md), #523) |

Each case declares options (e.g. element count, GBDT trees / depth, node count,
resolver strategy, calibration on/off) so a run is tunable from the panel.

## Example scripts — the example programs ARE the tests

The **Example scripts** category (`TestCategory::Examples`) is the
[sim-Python example-script harness](example-scripts.md) (#523): each example program
is run end to end — **parse** (the [Python frontend](python-frontend.md)) → **IR** →
**gate** ([RULE-1](gate.md)) → **run** ([Node::run_graph](execution.md)) — and its
declared expectations asserted on the outputs, so the example programs *are* the
integration tests. A green script doubles as a canonical usage example; a negative
script proves a failure surfaces with the **offending statement**. The catalog
(`example_scripts()`) + the runner (`run_example_script`) live in zetta-core, so they
run headless (`tests/test_example_scripts.cpp`) and render in the panel like every
other case. See [example-scripts.md](example-scripts.md).

## Real multi-node, not a simulation

The distributed cases run on a `LoopbackCluster`: N real `Node` + `Cluster`
instances on `127.0.0.1`, brought up sequentially (each dials the actually-bound
addresses of the earlier ones, robust to busy ports) and waited until membership
converges — the same substrate the distributed gtest suites use. A SPMD case spawns
a thread per node calling the collective / distributed op concurrently, then asserts
every node agrees (and, for group-by, that the distributed result equals the
single-node ground truth). Cases are **self-contained**: they stand up their own
ephemeral nodes (own temp dirs + ports) and never disturb the app's live node.

## Self-contained data

The ML / governance / pipeline cases use an **in-code synthetic churn dataset**
(`testkit::sample_churn_dataset`) — the FedEx Freight + sibling subsidiaries
resolution case (TC-4), churn correlated to failed payments, and a couple of
deliberately post-`as_of` events (the leakage trap) — so the catalog runs from any
binary with no on-disk fixture. The **Custom dataset dir** field (a churn dataset
directory loaded via `load_churn_dataset`) overrides it for the data-driven cases.

The Parquet parity case follows the same rule by **embedding** its fixture: the
pyarrow-produced `saas_sample.parquet` bytes (base64) + the golden JSON live in
`src/core/testkit/parquet_fixture_data.hpp`, generated by
`scripts/gen-parquet-fixture.py` from the same on-disk fixture the unit test reads
(one source of truth, no foreign runtime at build/CI — ADR-17).

No new JSON / wire format is introduced — custom-data ingestion rides the existing
schema-validated [churn dataset](churn-dataset.md) boundary, and every case calls the
already-validated engine boundaries.

## The headed panel

**Operations → Test suite** (`App::draw_test_suite`) renders the catalog grouped by
category (left), and a detail pane (right) with the selected case's description, its
options rendered as widgets, the custom-data field, a **Run test** button, and the
last result — an outcome badge (semantic dot + word), the summary, a mono metrics
table, and the log. A case runs on a **worker thread** (so the distributed cases'
blocking convergence waits never freeze the UI); the result + live log are written
under a mutex and rendered as they arrive. Following the Orama design system: sentence
case, mono for ids / counts / metrics, semantic green/amber/red for status, a charcoal
primary Run button.

## Adding a case

A case is a `TestCase` appended to the suite in one of the `src/core/testkit/cases_*.cpp`
`register_*_cases(TestSuite&)` functions (assembled by `TestSuite::builtin()`):

```cpp
suite.add(TestCase{
    .id = "compute.cpu.histogram",        // stable, dotted, unique
    .category = TestCategory::Compute,
    .name = "CPU device: histogram",
    .description = "Bin a vector on the device and check the counts host-side.",
    .options = {TestOption::integer("count", "Element count", 4096, 1, 2000000)},
    .run = [](const TestOptions& options, TestContext& context) -> TestResult {
        // ... call the real engine ...
        if (/* mismatch */) return TestResult::fail("histogram diverged from host");
        TestResult result = TestResult::pass("histogram matched the host");
        return result.metric("bins", "16");
    },
});
```

Guidelines: a case is **non-throwing-by-contract** (the runner catches anything and
reports `Error`, but prefer the non-throwing engine APIs and `TestResult::fail`);
**self-contained** (stand up your own ephemeral `Node` / `LoopbackCluster` via the
[harness](../include/zetta/testkit/harness.hpp), never touch the app's live node);
and **deterministic** (no wall-clock / RNG in the assertion). Report what happened
with `metric(...)` + `context.emit(...)`. A distributed case spins a thread per node
calling the collective / op concurrently (the SPMD pattern). Then add an assertion in
`tests/test_testkit.cpp` so the case is covered headless. Never call a real cloud
(the permission-gated rule) — inject a fake control plane instead.

## Inventory: candidate cases to add

The catalog is a representative slice today; the engine has far more that could be
surfaced as runnable, parameterized cases. This is the backlog, by category. Markers:
**(pure)** host-only, no infra · **(node)** needs a scratch `Node` · **(cluster)**
stands up a real loopback cluster · **(coordinator)** needs a `JobCoordinator`
(recorded `ComputeJob`) · **(GPU)** needs `-DZETTA_ENABLE_CUDA=ON` · **(fake seam)**
needs a test fake exposed to the testkit.

### Compute & device
- `compute.cpu.histogram` — bin counts vs host (pure)
- `compute.cpu.gemm` / `compute.cpu.axpy` — float matmul / axpy; reject integer dtype (pure)
- `compute.cpu.scan` — inclusive / exclusive prefix scan (pure)
- `compute.cpu.gather_scatter` — gather / scatter, out-of-range rejection, deterministic duplicate-index scatter (pure)
- `compute.cpu.dtype_guards` — integer-dtype rejection for sqrt/exp/log/gemm, safe integer divide (pure)
- `compute.device_registry` — enumerate / report / select (accelerator-first, CPU fallback, `require_kind` / `min_memory_bytes`) (pure)
- `compute.cuda.parity` — run every operator on CPU **and** CUDA and assert they agree (today's case only reports the build state) (GPU)
- `compute.work_stealing` — local per-worker-deque steal balance, `parallel_map` order, steal stats (pure)
- `runtime.parallel_for` — data-parallel tiling correctness + phase alignment (pure)

### DDF dataframe ops
- `ddf.project` — passthrough / unary (`map`) / binary (`elementwise`) computed columns (pure)
- `ddf.as_of` — point-in-time windowed aggregation; assert the post-`as_of` leakage trap is dropped (pure)
- `ddf.sort` / `ddf.concat` / `ddf.slice` — frame-shaping ops (pure)
- `ddf.join` — single-node hash / sort-merge inner equi-join (pure)
- `ddf.featurize` — `point_in_time_featurize` over the churn dataset vs a leakage-free ground truth (node)
- `ddf.distributed_sort` — sample-sort across the cluster; globally range-ordered partitions (cluster)
- `ddf.distributed_join` — repartition + local join; single-node == multi-node (cluster)
- `ddf.group_by_job` / `ddf.featurize_job` — the recorded `ComputeJob` path (appears in `system.zetta_jobs`) (coordinator)
- `query.group_by_sql` — `SELECT … GROUP BY` routed through the DDF engine (node)

### Machine learning
- `ml.score` — `score_gbdt` / rank-at-risk: calibrated risk, descending order, `top_k` (pure)
- `ml.calibration_metrics` — Brier / ECE reliability over predicted probabilities (pure)
- `ml.model_roundtrip` — `GbdtModel` JSON-Schema boundary: serialize weights, read back, re-validate (pure)
- `ml.playbook` — `generate_playbook` deterministic driver→action→owner + frozen expected-retained-value priority + the offline rules-engine narrator (pure)
- `ml.native_oracle` — load the committed golden oracle + the analytic closed-form GBDT-split / TreeSHAP micro-tests (pure)
- `ml.gbdt_distributed` — single-node == multi-node trees (gradient-histogram allreduce) (cluster)
- `ml.tree_shap_distributed` — the `explain` job: per-entity SHAP + the population mean-|φ| ranking, single == multi (cluster / coordinator)
- `ml.train_job` / `ml.score_job` / `ml.explain_job` — the recorded `ComputeJob` paths that register a `Model` (coordinator)

### Distributed & cluster
- `cluster.broadcast` / `cluster.gather` / `cluster.barrier` — the other collectives (cluster)
- `cluster.signal_plane` — request/response RPC, events, tasks over the cluster transport (cluster)
- `cluster.work_stealing_distributed` — cross-node steal; elastic mid-job membership (join / leave / lost-node re-exec) (cluster)
- `cluster.job_coordinator` — a `JobRound` body: `parallel_map` + collectives at a round boundary + lifecycle (cluster / coordinator)
- `cluster.replication` — CRDT convergence: create DDL + rows on A, converge on B; versions match (cluster)
- `cluster.large_object_replication` — body transfer (`LOREQ`/`LORES`), sha256-verified before write (cluster)
- `cluster.self_heal` — drop a peer, re-dial, restarted-node ghost replacement (cluster)
- `cluster.uc1_pipeline_multinode` — the SPMD UC-1 pipeline across N nodes (the deferred multi-node flagship) (cluster / coordinator)

### Governance, gate & orchestration
- `governance.capability_ttl` — TTL'd capability expiry with a deterministic `now` (node)
- `governance.scope_replication` — a key replicates to a peer and enforces identically (cluster)
- `gate.schema_type` / `gate.data_leakage` — the #309 checks (field/type mismatch; ML leakage / point-in-time) (pure)
- `gate.structural` — non-empty / acyclicity / legal stage transitions (pure)
- `governance.entity_resolver` — the resolver itself: FullName vs Brand, `normalize_entity_name`, the "Umbrella Logistics" near-collision (pure)
- `governance.intent_compiler` — NL intent → churn / generic skeleton, deterministic, round-trips through `ir::from_json` (pure)
- `governance.n8n` — import / compile an n8n workflow JSON → artifact input / ExecutionGraph IR (node)
- `governance.deployment` — `deploy_graph` + the reconcile / self-heal control loop (start, kill, heal a run) (node)
- `governance.burst_approval_gate` — an over-cap burst blocked pending approval + audited (`provision_requested`) (node)

### Provisioning (no real cloud calls)
- `provision.local` — `LocalProvisioner` best-effort select over the live cluster fleet (cluster)
- `provision.node_capability` — `NodeCapability::satisfies` (cores / memory / GPU kind + throughput) + the JSON boundaries (pure)
- `provision.autoscaler` — `JobAutoscaler` tick loop (observe → `ScalingPolicy::evaluate` → provision / teardown, hysteresis, cost-cap shed) against a **fake** `Provisioner` (fake seam)
- `provision.cloud_burst` — the Azure / AWS / GCP / OCI / k8s / bare-metal burst lifecycle (provision + mint / teardown + revoke) against a **fake** control plane — never a real CLI (fake seam)
- `provision.scaling_policy` — `evaluate()` per objective (minimize-cost / runtime-target / data-shape) + the hard cost-cap shed (pure)

### Storage, persistence & primitives
- `storage.jsonl_store` — round-trip + 10k-per-chunk chunking (node)
- `storage.import_records` — JSON / JSONL → table rows (array → many, object → one) (node)
- `storage.large_object` — import / export / read / unlink + sha256 fingerprint (node)
- `storage.integrity` — the preflight check: detect a corrupt large-object body + dangling references (node)
- `storage.persistence` — restart a node and confirm the tree (projects / runs / models / jobs / principals / audit) reloads from disk (node)
- `primitives.sha256` / `primitives.uuid` / `primitives.timestamp` — the self-contained primitives (pure)

### Query layer & catalog
- `query.ddl_dml` — CREATE / DROP DATABASE + TABLE, INSERT, SELECT with a `WHERE` comparison (node)
- `query.system_tables` — `system.zetta_nodes` / `zetta_deployments` / `zetta_models` / `zetta_jobs` computed at query time (node / cluster)
- `solution.catalog` — the built-in AI Solution Library blueprints parse + schema-validate + id-uniqueness (pure)
- `solution.churn` — `run_churn_solution`: the catalog churn blueprint on the real engine → a real `Model` + ranked list + drivers (coordinator)

Most **(pure)** and **(node)** cases are drop-in (same shape as the existing ones).
The **(fake seam)** provisioning cases need a small refactor to expose a fake control
plane / `Provisioner` into the testkit (the test fakes live in `tests/` today) — and
must **never** call a real cloud (the permission-gated rule). The **(GPU)** parity
case only runs on a CUDA build.

## Run it

```bash
# headless (every representative case, including the real loopback-cluster cases):
ctest --test-dir build -R Testkit --output-on-failure

# interactive: launch the headed app and open Operations -> Test suite.
./build/bin/zetta
```
