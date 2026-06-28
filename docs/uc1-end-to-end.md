# UC-1 end-to-end (gated + governed)

The make-or-break integration test for **UC-1 churn** (issue #399; realizes
**UC-1**, satisfies **TP-1**) — part of the native distributed accelerated-compute
pass (#264). It drives the *whole* churn pipeline on the committed
[synthetic fixture](churn-dataset.md) in one test and asserts the three acceptance
properties together: a **calibrated ranked at-risk list**, **division-correct
entity resolution**, and a **gated, governed run**.

```
tests/test_uc1_end_to_end.cpp   the integration test (two cases)
```

It composes pieces that already landed and are unit-tested in isolation — it adds
no new engine code, only the end-to-end wiring + assertions:

- the [synthetic churn dataset](churn-dataset.md) + loader (`load_churn_dataset`);
- the [intent compiler](intent-compiler.md)'s churn skeleton (`compile_intent`);
- [entity resolution](entity-resolution.md) (`entity_resolution_attribute`) + the
  [entity-resolution gate](entity-resolution-check.md) inside
  [`default_gate()`](gate.md);
- the real [churn StepExecutor](churn-step-executor.md) (`ChurnStepExecutor`) over a
  started [job coordinator](job-coordinator.md), which runs
  [featurize](ddf-featurize.md) → [train GBDT](gbdt.md) → [score/rank](gbdt-score.md)
  → [explain (TreeSHAP)](tree-shap.md) → retention playbook, each heavy step a
  recorded [`ComputeJob`](compute-jobs.md);
- [key-scoped access](access-control.md) (`Principal` / `AccessKey` / `Scope` /
  `Capability`) + the immutable [audit trail](audit-trail.md).

## What the pipeline does

```
synthetic data
  → featurize   (point-in-time, leakage-free, entity-resolved)
  → train       (distributed histogram GBDT, binary-logistic)
  → explain     (exact TreeSHAP → per-entity + population churn drivers)
  → rank        (calibrated risk, highest first → the at-risk list)
  → playbook    (join the at-risk list with each account's dominant driver)
```

The flow is **gated** (the correctness gate must pass before anything is placed)
and **governed** (the pipeline authorizes against a scoped, capability-bearing key,
and every governed action lands in the audit trail).

## The two cases

### 1. `GatedGovernedRunProducesCalibratedRankedListAndDrivers`

The happy path, asserting all three acceptance properties at once:

1. **Gate first (RULE-1).** The churn graph carries the dataset's
   division-correct `entity_resolution` annotation (resolved from the comms
   mentions with the `FullName` strategy) on its clean / feature-build step.
   `default_gate().run(graph)` **passes** and does not block deploy, and the
   `entity_resolution` check is present and `Pass` — the FedEx siblings were kept
   apart, so the run is allowed to proceed.
2. **Governed access.** A `retention-pipeline` service principal holds a key
   scoped to the `churn` project + database with `orchestration:start_run` and
   `data:read` capabilities. The run authorizes against the key before acting:
   in-scope resource + the `start_run` capability are allowed; an out-of-scope
   project, an unknown key, and an ungranted right are denied (deny-by-default).
   The gate having passed and access granted, the graph is placed with
   `deploy_graph` (a governed, audited deploy).
3. **Real compute + artifacts.** The `ChurnStepExecutor` runs the graph and
   produces a `Run` that is `Succeeded` and schema-valid, whose outputs reference:
   - a **real registered `Model`** (kind `classifier`, resolvable via
     `find_model`);
   - a **calibrated, ranked at-risk list** — one row per account, highest risk
     first, every risk a probability in `[0, 1]` (the binary-logistic
     `predict_proba` is the calibrated churn risk; see [gbdt-score.md](gbdt-score.md));
   - **drivers** — exact TreeSHAP makes `failed_payments` the dominant churn driver
     (per-entity + the population ranking);
   - a **retention playbook** pairing each at-risk account with its dominant driver.
4. **Audited.** Every heavy step ran as a recorded `ComputeJob`
   (featurize / train / score / explain), all `Succeeded`, and the immutable audit
   trail captured the whole flow — the key-scoped access decisions, the deploy, and
   each job's `created` + `finished` events. Nothing in the pipeline ran un-audited.

### 2. `ConflatingResolutionBlocksTheGatedRun`

The make-or-break negative. The same churn graph carries a `Brand`-strategy
resolution annotation, which **conflates** the FedEx siblings onto one account —
the canonical TC-4 failure. `default_gate().run(graph)` now **fails** and blocks
deploy (RULE-1), with the `entity_resolution` check `Fail`. The run never happens:
no model is trained, no compute is placed. This is the teeth behind
"division-correct resolution" — a buggy resolver cannot ship.

## Distribution

The test runs single-node (one loopback `Node` + `Cluster` + `JobCoordinator`),
which keeps it fast and deterministic. The underlying jobs' **single-node ==
multi-node equivalence** is proven in the per-op suites (featurize / train / score
/ explain) and the [native correctness oracle](native-oracle.md) (single-node ==
2-node == 3-node), so the end-to-end result is the same on a multi-node cluster —
this test asserts the *composition*, not the distribution, which is covered
elsewhere.

## Run it

```bash
ctest --test-dir build -R Uc1EndToEnd --output-on-failure
```
