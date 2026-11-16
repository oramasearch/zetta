# UC-1 churn end-to-end (gated + governed) · `pipeline.uc1_end_to_end`

**Category:** UC-1 pipeline · **Source:** `src/core/testkit/cases_pipeline.cpp` · **Engine doc:** [uc1-end-to-end.md](../../../use-cases/uc1-end-to-end.md)

The flagship pipeline case: the whole UC-1 churn pipeline made interactive (the
#399 end-to-end). Synthetic data → featurize (point-in-time, entity-resolved) →
train GBDT → explain (TreeSHAP) → ranked at-risk + drivers → playbook, behind the
real [`ChurnStepExecutor`](../../../use-cases/churn-step-executor.md), with the
[correctness gate](../../../pipeline/gate.md) passing first (RULE-1) and the run governed under
[key-scoped access](../../../governance/access-control.md) + the [audit trail](../../../governance/audit-trail.md).

## What it does

1. **Compiles + annotates the churn graph.** `compile_intent("reduce churn and
   improve retention")` builds the churn skeleton, and the dataset's
   [entity-resolution](../../../use-cases/entity-resolution.md) annotation is set on the
   `build_features` clean step — resolved with the `FullName` strategy by default,
   or the coarse `Brand` strategy when the `resolver` option selects it.
2. **Gates first (RULE-1).** `default_gate().run(graph)` runs before anything is
   placed. With `FullName` the Contoso siblings are kept apart, the gate passes, and
   the pipeline proceeds. With `Brand` the resolver conflates the siblings onto one
   account, so the gate **fails and blocks deploy** — and the case passes by
   asserting the run never happens (the governed-correct outcome).
3. **Governs.** A `retention-pipeline` service principal holds a key scoped to the
   `churn` project + database with `orchestration:start_run` + `data:read`
   capabilities. The case authorizes against it (in-scope allowed, out-of-scope
   denied, deny-by-default), then `deploy_graph` places the graph (a governed,
   audited deploy).
4. **Runs the real pipeline** through the one public churn runner
   ([`run_churn_solution_on_node`](../../../use-cases/churn-solution.md), #637) over the
   node-owned [`JobCoordinator`](../../../compute/job-coordinator.md) (the node
   `attach_cluster`s its loopback cluster): a
   [`ChurnStepExecutor`](../../../use-cases/churn-step-executor.md) runs
   featurize → train (+ optional calibration) → score/rank → explain (TreeSHAP) →
   playbook, each heavy step a recorded [`ComputeJob`](../../../compute/compute-jobs.md),
   registering a real [`Model`](../../../ml/models.md).

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ UC-1 pipeline → UC-1 churn end-to-end (gated + governed)**. Set the options in the
modal and click **Run**; the result pane shows pass/fail, the metrics (accounts, top
risk, top driver, jobs succeeded, audit events, calibrated), and the log.

**Headless:** the case is exercised (both the green run and the blocked run) by
`tests/testkit/test_testkit.cpp::Uc1PipelineGatedAndGoverned`, which runs the `resolver` at
`full_name` (passes by running) and at `brand` (passes by the gate blocking):

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.Uc1PipelineGatedAndGoverned' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `resolver` | full_name (runs) | full_name · brand | `full_name` runs the whole pipeline; `brand` conflates the Contoso siblings so the gate blocks the run (RULE-1) |
| `trees` | 30 | 1–300 | GBDT trees |
| `depth` | 3 | 1–10 | GBDT max depth |
| `calibrate` | false | bool | fit held-out Platt calibration on the model |
| `top_k` | 0 | 0–1000 | at-risk list cap (0 = all accounts) |

This case is single-node (one loopback `Node` + `Cluster`, the node owning its `JobCoordinator`); the
single-node == multi-node equivalence is proven in the Distributed cases (see
[`cluster.uc1_at_scale`](cluster.uc1_at_scale.md)).

## What it asserts (pass criteria)

For the `full_name` (running) path:

- A real registered [`Model`](../../../ml/models.md) (kind `classifier`, resolvable via
  `find_model`), a `Succeeded` + schema-valid [`Run`](../../../pipeline/runs.md).
- A **calibrated, descending at-risk list** — one row per account (or `top_k`), every
  risk a probability in `[0, 1]`, sorted highest first, and a **playbook** covering
  the whole list.
- **Drivers** — a non-empty top churn driver + a non-empty driver ranking (exact
  TreeSHAP).
- **Real compute** — every heavy step ran as a recorded
  [`ComputeJob`](../../../compute/compute-jobs.md) and all succeeded.
- **Gated + governed** — the gate passed, the key-scoped access decisions were correct
  (in-scope allowed, out-of-scope denied), and the [audit trail](../../../governance/audit-trail.md)
  captured the flow (one `graph_deployed`, at least two `data_access`, and each job's
  `created` + `finished`).

For the `brand` (blocked) path: the gate **failed and set `block_deploy`**, and the
pipeline never ran (metrics `gate=blocked`, `pipeline=not run`) — the teeth behind
division-correct resolution.

## Related

- Engine: [uc1-end-to-end.md](../../../use-cases/uc1-end-to-end.md) ·
  [churn-step-executor.md](../../../use-cases/churn-step-executor.md) ·
  [gate.md](../../../pipeline/gate.md) · [entity-resolution.md](../../../use-cases/entity-resolution.md) ·
  [access-control.md](../../../governance/access-control.md) ·
  [audit-trail.md](../../../governance/audit-trail.md)
- Sibling cases: [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md) ·
  [`examples.churn_pipeline`](examples.churn_pipeline.md) ·
  [`governance.entity_resolution_gate`](governance.entity_resolution_gate.md)
