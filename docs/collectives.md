# Collectives (allreduce / broadcast / barrier / gather)

**Collective communication** over the [signal plane](signal-plane.md) — the
primitive the native distributed accelerated-compute pass for UC-1 churn (#264,
ADR-28 / ARC-10) uses to combine per-node compute *partials* across the cluster:
gradient histograms, partial aggregates, and the like. `zetta::Collectives`
(`include/zetta/collectives.hpp`) implements four collectives, modeled on MPI:

| collective  | what it does                                                                 |
|-------------|------------------------------------------------------------------------------|
| `allreduce` | every node contributes a vector; **every** node receives their elementwise reduction (sum / product / min / max) |
| `broadcast` | the root node's vector is delivered to **every** node                        |
| `barrier`   | every node blocks until **all** have arrived, then all proceed               |
| `gather`    | every node contributes a vector; the **root** receives all of them, ordered (classic MPI gather — the result lands only at the root) |

It rides entirely on the signal plane's RPC (`Cluster::request`) — no new wire
verb, no new transport. It is **ephemeral**, like every signal: a collective is a
transient coordination between live peers, not replicated state.

## Determinism — the whole point

A reduction must produce **bit-identical results on every node, independent of
node order**. Floating-point addition is not associative, so summing the same
vectors in a different order can yield different bits. Collectives guarantees a
single answer two ways:

1. **A leaderless, deterministic root.** The participants are this node plus its
   connected peers; the *root* is the **lexicographically-smallest node id**.
   Every node computes the same root with no election.
2. **Reduce once, at the root, in node-id order.** The root gathers every
   contribution, reduces them in sorted node-id order, and pushes the **single**
   result back to everyone. Because all nodes receive the root's exact bytes,
   they cannot disagree — and because the reduction order is fixed, re-running
   the round reproduces the same value.

This is why the root reduces and broadcasts rather than each node reducing its
own copy.

## Using it

Construct a `Collectives` over a node's `Cluster`; it registers itself as the
cluster's `SignalHandler`. Every participant calls the same collective with the
same `tag` (the round's name) — the calls rendezvous and return together.

```cpp
zetta::Collectives collectives{cluster};   // registers the signal handler

// allreduce: every node passes its partial; every node gets the sum.
std::optional<std::vector<double>> total =
    collectives.allreduce("grad-epoch-7", local_gradient, zetta::CollectiveOp::Sum);
if (total) train_step(*total);

// broadcast: the root's weights go to everyone.
collectives.broadcast("weights-v1", new_weights);   // root supplies; others receive

// barrier: nobody proceeds until all arrive.
collectives.barrier("checkpoint");

// gather: the root ends up with every node's vector, ordered by node id.
std::optional<std::vector<std::vector<double>>> all = collectives.gather("collect", local);
```

Every entry point takes an optional `timeout` (default
`Collectives::kDefaultTimeout`, 10 s). All return an empty optional / `false` on
failure — see *Failure semantics* below.

A node that also needs to handle **its own** signals (events, tasks, non-collective
requests) passes that handler as the constructor's `delegate`; `Collectives`
forwards everything it doesn't recognize to it, and restores it on destruction:

```cpp
zetta::Collectives collectives{cluster, &my_task_handler};
```

## How a round runs

A round is a **star** (a depth-1 tree): the root gathers from all participants
and broadcasts the result. Driven from the calling (application) thread, which
blocks until the round completes:

```
non-root node                      root node (lowest id)
─────────────                      ─────────────────────
allreduce(tag, local) ───────────► on_request("collective.contribute")
  request(root, contribute, local)   store contribution, ack, wake the waiter
  (blocks waiting for the result)
                                   allreduce(tag, local)
                                     wait until ALL contributions arrived
                                     reduce in node-id order
                                     request(peer, "collective.result", result) ──┐
on_request("collective.result") ◄──────────────────────────────────────────────┘
  store result, ack, wake the waiter
  return result                    return result
```

Two RPC methods carry the round: `collective.contribute` (non-root → root) and
`collective.result` (root → non-root). Both payloads are a
[`CollectiveMessage`](#wire-format--the-schema-boundary).

### Threading

This mirrors the signal plane's contract exactly (see
[signal-plane.md](signal-plane.md#threading-model)):

- **The collective is driven from an application thread** — the one that calls
  `allreduce()` / `broadcast()` / `barrier()` / `gather()`. It blocks on a
  condition variable until the round completes or the timeout fires.
- **The signal handler never blocks.** `Collectives::on_request` runs on the
  cluster's network thread; it only records an arrival or a result and wakes the
  waiting application thread, then acks. Heavy work never happens there, and no
  `request()` is ever issued from the handler thread (which would deadlock).
- Because RPC carries every message, a peer that is unreachable fails the round
  **fast** (the contribute / result `request()` returns `ok = false`) instead of
  hanging out to the timeout.

Late or early messages are safe: the handler stores arrivals/results in a
per-`tag` slot, and the waiter checks that slot under the lock before sleeping,
so a result that lands before its waiter starts waiting is not lost.

## Failure semantics

- **Length mismatch** — `allreduce` returns `nullopt` at the root when
  contributions disagree on length (the root reduces in order and detects it).
  The contributing peers never receive a result and time out.
- **Unreachable peer** — the contribute / result RPC returns `ok = false`; the
  round fails fast for the node that hit it.
- **Timeout** — a node that never sees all the arrivals (root) or the result
  (non-root) within the timeout returns `nullopt` / `false`.
- **Singleton cluster** — a node with no peers is its own root: `allreduce`
  returns its own vector, `broadcast` returns the value, `barrier` returns
  `true`, `gather` returns just its own vector. Useful for tests and degenerate
  deployments.

## Wire format + the schema boundary

The two RPC methods carry a `zetta::CollectiveMessage` as the signal's opaque
payload — a compact JSON object:

```json
{"tag":"grad-epoch-7","from":"<node-id>","values":[1.5,-2.0,3.25]}
```

`tag` names the round; `from` is the sender's node id (on a contribution);
`values` is the per-node vector (omitted/empty for a barrier). It is **validated
at the boundary** on receipt, exactly like the [signal envelope](signal-plane.md#wire-format--the-schema-boundary)
and the [Run record](runs.md): against a draft-07 **JSON Schema**
(`schemas/collective_message.schema.json`, mirrored embedded in
`src/core/collectives.cpp`, via `pboettch/json-schema-validator`). `tag` is
required, no unknown fields are allowed, and `values` must be an array of
numbers — a malformed payload is rejected with a path-qualified reason and the
RPC fails, never corrupting a round. `encode_collective_message` /
`decode_collective_message` are the serialization seam (non-throwing, matching
`decode_signal`).

## Where it sits

```
include/zetta/collectives.hpp   Collectives + CollectiveOp + CollectiveMessage
                                + encode/decode (the wire seam)
src/core/collectives.cpp        the round logic + the JSON-Schema boundary
schemas/collective_message.schema.json   the draft-07 schema (mirror)
```

`Collectives` depends only on `zetta::Cluster` (for `request()` and the
participant list) and `zetta::Signal` (the handler base). It introduces no
backend types and no new transport — it is a pure consumer of the signal plane.

## Limits / future

- **Star, not yet a tree/ring.** R1 reduces at a single root (fan-in = number of
  participants). A k-ary tree or ring reduction — to cut the root's fan-in at
  large scale — is a later optimization behind this same API; the determinism
  guarantee (a fixed reduction order, one broadcast result) carries over.
- **Membership must be consistent.** Participants are derived from each node's
  live peer set, so the collective assumes a **fully-connected** cluster (every
  node sees every other). On a partitioned/skewed membership a round times out.
  Routing membership through the replicated document (the standing migration
  debt in [replication.md](replication.md)) would make the participant set
  authoritative.
- **One round per `tag` at a time.** A `tag` names a single in-flight round; its
  state is dropped when the round completes. Reuse a fresh tag per round (e.g.
  `"grad-epoch-<n>"`).
- **Best-effort transport.** Inherited from the signal plane — no retries or
  persistence; a peer that drops mid-round fails that round (the caller retries
  at a higher level).

## Observability

`Collectives` carries two cumulative, thread-safe meters for the live-pipeline
read views ([pipeline-instrumentation.md](pipeline-instrumentation.md)):

- `wire_bytes()` — total collective-plane payload bytes sent/received (the
  "bytes moved" the compute-near-data path keeps small).
- `rounds_completed()` — the number of rounds (allreduce / broadcast / barrier /
  gather) that succeeded; a timed-out or unreachable-peer round does not count.

Both surface on the running node via `system.collective_rounds`, so the GUI can
watch a distributed run advance.

## See also

- [pipeline-instrumentation.md](pipeline-instrumentation.md) — the
  `system.collective_rounds` read view over the meters above.
- [signal-plane.md](signal-plane.md) — the ephemeral RPC / event / task layer
  this is built on.
- [compute-device.md](compute-device.md) — the operator seam (`reduce`,
  `histogram`, …) that produces the partials collectives combine.
- [cluster.md](cluster.md) — the equal-peer transport underneath.
