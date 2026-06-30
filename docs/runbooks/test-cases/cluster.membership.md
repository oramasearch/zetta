# Cluster membership · `cluster.membership`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_distributed.cpp` · **Engine doc:** [cluster.md](../../cluster.md)

Bring up an N-node loopback cluster over TCP and confirm membership converges:
every node sees the other N-1 peers. The simplest distributed case, it proves the
real equal-peer cluster forms before the heavier cases build on it.

## What it does

1. **Stands up N real nodes.** `LoopbackCluster(nodes)` brings up N
   [`zetta::Cluster`](../../cluster.md)s on loopback TCP (`127.0.0.1`): a real,
   fully-connected, equal-peer cluster, not a simulation. It waits for them to
   connect within the bring-up window.
2. **Confirms convergence.** It checks `cluster.connected()`, then reads each
   node's `peer_count()`, so the result reports how many peers every node sees.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → Cluster membership**. Set the options in the modal
and click **Run**; the case runs on a worker thread and stands up a real
multi-node loopback cluster, so the UI stays responsive. The result pane shows
pass/fail, the metrics (the node count and each node's peer count), and the log.

**Headless:** the case is exercised (at `nodes=2`) by
`tests/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | cluster size: the number of loopback nodes brought up |

## What it asserts (pass criteria)

- The N nodes formed a **fully-connected cluster** within the bring-up window
  (`cluster.connected()`); a cluster that fails to converge fails the case.
- Every node sees the other **N-1 peers** (the per-node peer count is in the
  metrics).

## Related

- Engine: [cluster.md](../../cluster.md) · [signal-plane.md](../../signal-plane.md)
- Sibling cases: [`cluster.allreduce`](cluster.allreduce.md) ·
  [`cluster.group_by_consistency`](cluster.group_by_consistency.md) ·
  [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md)
