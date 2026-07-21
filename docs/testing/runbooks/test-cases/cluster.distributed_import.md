# Distributed connector import (co-launch) · `cluster.distributed_import`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_connect.cpp` · **Engine doc:** [distributed-connector-ingest.md](../../../data/distributed-connector-ingest.md)

The **cross-node connector ingest** running for real: one query submitted once for
the whole cluster, every node pulling its own tile straight from the source and
landing it locally. Tiling a result set and then fetching every tile on the
coordinator is a parallel loop, not a distributed algorithm — this case is what
tells the two apart
([#833](https://github.com/oramasearch/zetta/issues/833) section 7,
[#829](https://github.com/oramasearch/zetta/issues/829)).

The source is a **fake warehouse** installed as the process-wide default transport
for the life of the case (that is how a *peer* reaches a source — it never sees the
initiator's injected argument), and the previous transport is **restored**
afterwards, so running the case inside a live app leaves its real client in place.
The fake records, per calling **thread**, which chunks that thread read — which is
what makes "who fetched what" observable across the cluster.

## What it does

1. **Brings up an N-node cluster wired the way a live process is**
   (`LoopbackOptions{.attached = true}`): a `ReplicationManager` +
   `ReplicatedBlobStore` behind each node, each announced as a replicated member,
   and `Node::attach_cluster` called — which is what installs the `run.launch`
   listener the ingest is dispatched by.
2. **Saves the connection once** and waits patiently until every node resolves it —
   the credential is replicate-to-all precisely so nothing secret rides the launch.
3. **Starts a node loop per peer.** The initiator submits from the case thread, so
   its tile is attributable to that thread and each peer's to its own.
4. **Runs `run_distributed_connector_ingest`** — submit → co-launch → each node
   fetches only its own tile → each lands it as
   [partition-only](../../../data/partition-only-tables.md),
   [fetch-affinity](../../../data/fetch-affinity-placement.md) partitions it owns.
5. **Reads the dataset back** through the
   [scatter-gather SELECT](../../../data/scatter-gather-select.md), patiently, while
   the peers are still ticking.
6. **Stops the peer loops**, then reads each node's tree (owner-thread-only) to
   check ownership and the replicated document.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Distributed & cluster → Distributed connector import (co-launch)**. Set the
options in the modal and click **Run**; the case runs on a worker thread and stands
up its own cluster, so the UI stays responsive.

**Headless:** the case is exercised (at `nodes=2`, `chunks=6`, `rows=4`) by
`tests/testkit/test_testkit.cpp::ConnectorCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ConnectorCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 2–6 | cluster size — every one of them is a fetch participant |
| `chunks` | 9 | 2–256 | independently-fetchable result partitions the source advertises; **raised to `nodes + 1`** if lower, because chunk 0 arrives inline with the submission and the fan-out needs at least one fetchable chunk per node |
| `rows` | 6 | 1–2000 | rows per source chunk (total rows = `chunks × rows`) |

## What it asserts (pass criteria)

- **It was actually co-launched.** `coordinated` is true and every node is a
  participant — a degrade to the local path fails the case, since that is the
  parallel loop this work exists to replace.
- **Exactly once, cluster-wide.** The statement is submitted **once**; chunk 0
  (which arrived inline) is never re-read; every other chunk is read **exactly
  once**. The per-thread tiles are **disjoint** and their union is the whole
  fetchable result set, and **at least two nodes fetched** — one node pulling
  everything is the failure this asserts against.
- **Land where you pulled.** Each node holds exactly the rows of the chunks *it*
  fetched (the initiator additionally holds the inline chunk 0). Every catalog
  entry it wrote has **one owner** (a cached result set is RF=1), that owner is a
  participant, the owner is that node itself, and the entry's recorded placement is
  still `fetch_affinity` — a rebalance would have rewritten it. The row ranges
  account for exactly the rows that node landed, and the participants together own
  the whole result set once.
- **Nothing re-replicated.** Every node's replicated document holds **zero `row:`
  entries** for the table: the rows crossed the wire exactly once, from the source
  to the node that owns them.
- **The dataset reads back complete and correct.** The scatter-gather `SELECT`
  returns all `chunks × rows` rows, each id exactly once, and the summed `amount`
  matches the closed form over the same input.

## Metrics reported

`nodes` · `source chunks` · `chunks per node` (e.g. `3 / 3 / 2 (+1 inline on node
0)`) · `fetching nodes` · `rows` · `partitions written` · `planned partitions` ·
`bytes off the source` · `replicated row entries` · `statement submissions`.

## Not asserted here (deliberately)

- **A node dying mid-fetch fails the whole ingest.** Not asserted *here*, because
  this case is the success path — but it is now demonstrable in-app too, as
  [`cluster.import_participant_loss`](cluster.import_participant_loss.md). It needs
  no fault injection: the case owns its loopback nodes, and a node is made idle by
  not giving it a tick loop rather than by wedging it (#840). Also pinned at L1 by
  `tests/connect/test_distributed_ingest.cpp::AParticipantThatStopsMidFetchFailsTheWholeIngest`.
- **The concurrent, several-in-flight fetch within one node.** The fake transport
  is installed with no async half, so every node takes the documented serial
  degrade — which is what keeps each node's reads attributable to its own thread.
  `tests/connect/test_distributed_fetch.cpp` covers the in-flight fan-out.

## Related

- Engine: [distributed-connector-ingest.md](../../../data/distributed-connector-ingest.md) ·
  [connector-query.md](../../../data/connector-query.md) ·
  [fetch-affinity-placement.md](../../../data/fetch-affinity-placement.md) ·
  [partition-only-tables.md](../../../data/partition-only-tables.md) ·
  [scatter-gather-select.md](../../../data/scatter-gather-select.md) ·
  [partition-scheduler.md](../../../data/partition-scheduler.md)
- Sibling cases: [`cluster.connections`](cluster.connections.md) — the credential
  this ingest resolves · [`cluster.compute_near_data`](cluster.compute_near_data.md)
  — the read-side mirror of land-where-you-pulled ·
  [`cluster.partition_rebalance`](cluster.partition_rebalance.md)
