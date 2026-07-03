# Collectives: allreduce · `cluster.allreduce`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_distributed.cpp` · **Engine doc:** [collectives.md](../../../cluster/collectives.md)

Every node contributes a vector; an allreduce-sum over the signal plane returns
the identical global result on every node, checked against the closed form. It
proves the [collectives](../../../cluster/collectives.md) primitive the distributed compute
pass combines per-node partials with is deterministic and order-independent.

## What it does

1. **Brings up the cluster + collectives.** `LoopbackCluster(nodes)` stands up N
   real nodes; each node's [`zetta::Cluster`](../../../cluster/cluster.md) is wrapped in a
   [`Collectives`](../../../cluster/collectives.md).
2. **Contributes SPMD.** A thread per node builds a local vector of width 4 where
   element `j` of node `i` is `(i+1)·(j+1)`, then calls
   `allreduce(..., CollectiveOp::Sum)` so every node ends with the elementwise sum.
3. **Checks the closed form.** The expected element `j` is `(j+1)` times the node
   sum `N(N+1)/2`. Because the round is leaderless (the lexicographically-smallest
   node id reduces once in node-id order and broadcasts), every node ends with
   bit-identical bytes.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → Collectives: allreduce**. Set the options in the
modal and click **Run**; the case runs on a worker thread and stands up a real
multi-node loopback cluster, so the UI stays responsive. The result pane shows
pass/fail, the metrics (node count, vector width, `result[0]`), and the log.

**Headless:** the case is exercised (at `nodes=2`) by
`tests/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | cluster size: the number of nodes contributing to the round |

## What it asserts (pass criteria)

- Every node received an allreduce result (no node came back empty).
- Each element matches the **closed-form expected value** within `1e-6` on every
  node, so the global result is identical regardless of node order.

## Related

- Engine: [collectives.md](../../../cluster/collectives.md) ·
  [signal-plane.md](../../../cluster/signal-plane.md)
- Sibling cases: [`cluster.membership`](cluster.membership.md) ·
  [`cluster.group_by_consistency`](cluster.group_by_consistency.md) ·
  [`cluster.compute_near_data`](cluster.compute_near_data.md)
