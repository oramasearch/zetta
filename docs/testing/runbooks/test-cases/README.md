# Test-case runbooks

One runbook per **test-suite case** — the canned, parameterized cases in
[`zetta::testkit`](../../test-suite.md) that run **headless** (the `zetta-tests`
gtest binary) **and** in the headed **Operations → Test suite** panel. Each runbook
explains *what the case does*, *how to run it* (in the app + headless), *its
options*, and *what it asserts*.

## How to run any case

**In the app** (headed):

```bash
cmake --build build --target zetta        # build the GUI binary
./build/bin/zetta                         # launch (headed by default)
```

Then open **Operations → Test suite**, pick the category and case, set the options
in the modal, and click **Run**. The result pane shows pass / fail, the metrics, and
the log. Cluster cases stand up a real multi-node loopback cluster on a worker
thread (the UI stays responsive).

**Headless** (CI / no display): the catalog is validated by the gtest binary. Each
category maps to one gtest in `tests/testkit/test_testkit.cpp` (examples also in
`tests/testkit/test_example_scripts.cpp`):

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R '<gtest name>' --output-on-failure
```

There is **no per-case CLI runner** — headless coverage is the gtest, the GUI panel
is the interactive runner. The catalog itself lives in zetta-core (no SDL), so every
case is headless-runnable.

## The cases

### Compute & device — `tests/testkit/test_testkit.cpp::ComputeCasesPass`
- [`compute.cpu.elementwise`](compute.cpu.elementwise.md) — CPU device elementwise / map operators
- [`compute.cpu.reduce_sort`](compute.cpu.reduce_sort.md) — CPU device reduce + sort operators
- [`compute.cuda.conformance`](compute.cuda.conformance.md) — CPU↔CUDA operator conformance (skipped on a CPU-only build)

### DDF dataframe ops — `DdfCasesPass` / `ParquetReaderParityCasePasses`
- [`ddf.scan_filter`](ddf.scan_filter.md) — scan a table into a DataFrame + filter
- [`ddf.group_by`](ddf.group_by.md) — groupby-aggregate over a DataFrame
- [`ddf.parquet_reader_parity`](ddf.parquet_reader_parity.md) — native Parquet reader parity vs a pyarrow golden (TC-9)

### Machine learning — `MlCasesPass`
- [`ml.gbdt_train`](ml.gbdt_train.md) — train a histogram GBDT to a target AUC
- [`ml.tree_shap`](ml.tree_shap.md) — exact TreeSHAP efficiency identity
- [`ml.calibration`](ml.calibration.md) — probability calibration (Brier / ECE improve, AUC preserved)

### Distributed & cluster — `DistributedCasesRunOnARealCluster`
- [`cluster.membership`](cluster.membership.md) — a real N-node loopback cluster converges
- [`cluster.allreduce`](cluster.allreduce.md) — allreduce returns the identical global result on every node
- [`cluster.group_by_consistency`](cluster.group_by_consistency.md) — distributed group-by == single-node
- [`cluster.compute_near_data`](cluster.compute_near_data.md) — a group-by over each node's owned partitions (TC-11)
- [`cluster.partition_rebalance`](cluster.partition_rebalance.md) — partitions rebalance on a node join (TC-10)
- [`cluster.featurize_partitions`](cluster.featurize_partitions.md) — near-data featurize over a partitioned table
- [`cluster.fulltext_index`](cluster.fulltext_index.md) — per-partition full-text index, scatter-gather ranking (TC-13)
- [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md) — the UC-1-at-scale capstone (TC-12)

### Governance — `GovernanceCasesPass`
- [`governance.access_control`](governance.access_control.md) — key-scoped access decisions (deny-by-default)
- [`governance.audit`](governance.audit.md) — the immutable audit trail
- [`governance.entity_resolution_gate`](governance.entity_resolution_gate.md) — the entity-resolution correctness gate (TC-4)
- [`governance.burst_identity`](governance.burst_identity.md) — bulk burst identity mint + teardown revocation (TC-6)

### UC-1 pipeline — `Uc1PipelineGatedAndGoverned`
- [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md) — the UC-1 churn pipeline, gated + governed (#399)

### Example scripts — `ExampleScriptCasesRunGreen` / `tests/testkit/test_example_scripts.cpp`
- [`examples.churn_pipeline`](examples.churn_pipeline.md) — the full taxonomy churn pipeline (green)
- [`examples.minimal_train`](examples.minimal_train.md) — the smallest green sample (load → train)
- [`examples.method_chain`](examples.method_chain.md) — method chaining lowers to a linear path
- [`examples.uc1_at_scale`](examples.uc1_at_scale.md) — the UC-1-at-scale program (lowered shape + single-node run)
- [`examples.unterminated_string`](examples.unterminated_string.md) — negative: a syntax error surfaces the offending statement
- [`examples.no_pipeline_steps`](examples.no_pipeline_steps.md) — negative: a script with no calls is rejected

## See also

- [test-suite.md](../../test-suite.md) — the testkit catalog + the "adding a case" how-to
- [example-scripts.md](../../example-scripts.md) — the sim-Python example harness
