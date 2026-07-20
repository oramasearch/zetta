# Distributed import: a lost participant fails the whole ingest · `cluster.import_participant_loss`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_connect.cpp` · **Engine doc:** [distributed-connector-ingest.md](../../../data/distributed-connector-ingest.md)

The **all-or-nothing** property of the cross-node connector ingest, demonstrated in
the app rather than only at L1
([#840](https://github.com/oramasearch/zetta/issues/840)). A dataset assembled from
N nodes' tiles is only a dataset if every tile arrived; a result set missing one
node's stripe that is nonetheless reported as complete is worse than a failure,
because nothing downstream can tell.

## The mechanism: no fault injection at all

The obvious way to model a dead participant is to wedge a node, and
[`cluster.distributed_import`](cluster.distributed_import.md) declined to for good
reason — leaving a node deliberately broken is a poor thing to do to a panel run.

That turned out to be avoidable rather than unavoidable, on two observations:

- **The case owns its cluster.** `testkit::LoopbackCluster` stands up its own nodes
  in their own temp directories and tears every one of them down on destruction.
  The host process's live node — the desktop session, or the deployed node serving
  the panel — is never a member and is never touched.
- **"Dead" is the absence of a thread, not a broken node.** A node participates in
  a co-launched ingest by running a tick loop: `tick()` is what drains the
  `run.launch` queue and executes the co-fetch. A node that is simply never given
  one is a node that stays in the roster — connected, counted, a collective
  participant — and never fetches its tile or reports in. Nothing is broken, so
  nothing needs healing. It is the same mechanism the L1 test uses.

So the case is **safe, scoped and self-healing** by construction, not by cleanup.

## What it does

1. **Brings up an N-node cluster wired the way a live process is**
   (`LoopbackOptions{.attached = true}`), against a fake warehouse installed as the
   process-wide transport for the life of the case and restored afterwards.
2. **Saves the connection once** and waits until every node resolves it.
3. **Starts a node loop for every peer but the last.** The last is in the roster and
   does no work.
4. **Runs the ingest** with a short (8 s) coordination timeout, because the expected
   outcome *is* the timeout: the missing node's contribution to the `allreduce`
   never arrives, so the sum comes up short of the snapshotted roster.

## What it asserts

- **The ingest FAILS.** A partial result set is never reported as a complete
  dataset.
- **The reason tells the caller what to do** — it contains "re-run the ingest". A
  connector result set is a cache; re-running is the remedy.
- **No dataset is reported.** `result.table` is empty.
- **The node that never ran landed nothing** — it never fetched a tile, so it has no
  table to have kept.
- **The nodes that DID run kept nothing**, patiently: no `datasets.cached` table and
  no `cached` entry in `system.datasets` on any of them.

## Not asserted here (deliberately)

- **A node that was merely paused, then came back.** Only the running nodes are
  ticked while waiting for the cleanup to converge. Ticking the idle one models
  something different: a node that wakes, drains the launch it had queued, and
  **re-adopts the table after the cleanup already ran** — a late launch beating a
  completed cleanup. That is a real behaviour and worth its own case; it is not the
  behaviour of a node that died, which is what this one is about.
- **Several fetch requests in flight within one node.** The in-app fake transport
  is installed with no async half, so every node takes the documented serial
  degrade. `tests/connect/test_distributed_fetch.cpp` covers the in-flight fan-out
  at L1, and it stays there until the harness gains an async fake — the remaining
  half of [#840](https://github.com/oramasearch/zetta/issues/840).

## Metrics reported

`nodes` · `nodes doing work` · `source chunks` · `coordination timeout` ·
`statement submissions`.

## Related

- Engine: [distributed-connector-ingest.md](../../../data/distributed-connector-ingest.md)
  → *All or nothing*
- Sibling cases: [`cluster.distributed_import`](cluster.distributed_import.md) — the
  same ingest succeeding · [`cluster.connections`](cluster.connections.md) — the
  credential it resolves
- L1: `tests/connect/test_distributed_ingest.cpp::AParticipantThatStopsMidFetchFailsTheWholeIngest`
