# Signal plane (RPC / events / tasks)

The **signal plane** is a typed, *ephemeral* message layer carried over the same
equal-peer [cluster](cluster.md) transport that already moves replication gossip
and large-object bodies. It lets one node send another a message **addressed by
node id**, in three shapes:

- **Request / Response (RPC)** — call a peer and block until it replies.
- **Event** — fire-and-forget notification (to one peer, or broadcast to all).
- **Task** — fire-and-forget work envelope (the substrate for job dispatch).

It realizes **ADR-28 / ARC-10** and is the foundation the rest of the native
distributed compute pass builds on: collectives (allreduce/broadcast/barrier/
gather), the coroutine work-stealing scheduler, and the job coordinator all send
their messages through this layer.

> **Ephemeral — not replicated.** Unlike everything else a node agrees on, a
> signal does **not** go through the [replicated CRDT document](replication.md).
> It exists only for the lifetime of the message: a transient control/compute
> message between two live peers, gone if a peer is offline. This is the one
> deliberate exception to the "everything is replicated state" rule — a signal
> is not *state*, it's a *message*. Durable, agreed-upon state still belongs in
> the CRDT; signals carry the work and the coordination around it.

## The four kinds

`zetta::cluster::SignalKind` (`include/zetta/cluster/signal.hpp`), ordered
`request → response → event → task`. A `zetta::cluster::Signal` carries:

| field     | meaning                                                            |
|-----------|-------------------------------------------------------------------|
| `kind`    | one of the four above                                             |
| `id`      | correlation id (ties a Response to its Request)                   |
| `method`  | application-defined verb (the RPC method / event name / task type)|
| `payload` | opaque application bytes — the signal plane does **not** interpret them (often JSON, but any bytes) |
| `from`    | the sender's node id (filled in on receipt)                      |
| `ok`/`error` | a Response's success flag + failure reason                    |

## Using it

Wire a handler in and send from any thread:

```cpp
class MyHandler : public zetta::cluster::SignalHandler {
public:
    // RPC: return the response payload. Runs on the cluster network thread.
    zetta::cluster::SignalResponse on_request(const zetta::cluster::Signal& request) override {
        return {.ok = true, .payload = compute(request.payload)};
    }
    void on_event(const zetta::cluster::Signal& event) override { /* ... */ }
    void on_task(const zetta::cluster::Signal& task)   override { /* ... */ }
};

MyHandler handler;
cluster.set_signal_handler(&handler);   // any time; nullptr clears

// Request/response RPC — blocks until the peer replies, times out, or the peer
// is unreachable:
zetta::cluster::SignalResponse response =
    cluster.request(peer_node_id, "sum", "[1,2,3]", std::chrono::seconds(5));
if (response.ok) use(response.payload);

// Fire-and-forget:
cluster.send_event(peer_node_id, "heartbeat", payload);   // unicast
cluster.broadcast_event("epoch-start", payload);          // to every peer
cluster.send_task(peer_node_id, "map-shard", payload);    // a work envelope
```

`SignalHandler` has a safe default for each method, so a handler overrides only
the kinds it cares about. A node with **no** handler still answers requests — it
replies with a clear `"no handler"` error rather than hanging the caller.

### Threading model

- **Handlers run on the cluster's network thread.** Keep them cheap and
  thread-safe; offload heavy work to another executor — e.g. the
  [work-stealing scheduler](../compute/work-stealing-scheduler.md), whose `steal` /
  `steal_result` handlers only hand out / store task descriptors and run the
  actual compute on its own pool. An RPC handler returns its response
  synchronously.
- **Senders run on the caller's thread.** `request()` blocks the calling thread
  until the response, the timeout, or a peer disconnect — so do **not** call it
  from inside a handler (the network thread), which would deadlock. The
  fire-and-forget sends never block.
- Cross-thread hand-off is a mutex-guarded per-peer outbox plus a promise map for
  pending RPCs — producers never touch a connection or post a coroutine across
  threads (the coroutine backend's frame allocator isn't thread-safe). The
  per-link writer flushes the outbox on a short tick (`kOutboxPollInterval`,
  20 ms) so a signal leaves promptly while the heavier gossip/keepalive keeps its
  own ~0.5 s cadence.

### Failure semantics

`request()` returns `ok = false` with a reason — never hangs past the timeout:

- `"peer not connected"` — no current connection to that node id (fails fast).
- `"peer disconnected"` — the peer dropped while the request was in flight (all
  its pending requests fail immediately, no waiting out the timeout).
- `"request timed out"` — no response within the timeout.
- `"no handler"` — the peer has no handler (or its handler declined).

`send_event` / `send_task` return whether the target is currently connected and
`broadcast_event` returns how many peers it was queued to — all **best-effort**:
delivery of a fire-and-forget signal is not guaranteed (it is ephemeral).

## Wire format + the schema boundary

A signal travels as one new line-protocol frame alongside `SYNC` / `STATUS` /
`LOREQ` / `LORES` (see [cluster.md](cluster.md)):

```
ZETTA/1 SIGNAL <compact-json-envelope>
```

The envelope is a **single-line, compact JSON object** with the opaque payload
base64-encoded inside it (binary-safe, and a compact dump never contains a raw
newline, so it survives the newline-framed transport):

```json
{"kind":"request","id":"<uuid>","method":"sum","payload":"<base64>"}
{"kind":"response","id":"<uuid>","ok":true,"payload":"<base64>"}
{"kind":"event","method":"heartbeat","payload":"<base64>"}
```

The envelope is **validated at the boundary** on receipt, exactly like the
[Run record](../pipeline/runs.md) and the [audit trail](../governance/audit-trail.md): against a draft-07
**JSON Schema** (`schemas/signal_envelope.schema.json`, mirrored embedded in
`src/core/cluster/signal.cpp` — that embedded copy is the one enforced at runtime, via
`pboettch/json-schema-validator`). `kind` is a closed enum and no unknown fields
are allowed, then the kind-specific required fields (a request/response needs an
`id`; a request/event/task needs a `method`; a response needs `ok`) are checked
in code on top of the schema — the same "schema for shape, code for structural
rules" split as `ir::from_json`. A malformed or version-skewed envelope is
rejected with a path-qualified reason and **never reaches a handler**;
`decode_signal` is non-throwing (it returns `std::optional` + a reason), matching
zetta's no-exceptions-across-the-core-seam convention.

`zetta::cluster::encode_signal` / `zetta::cluster::decode_signal` (declared in
`include/zetta/cluster/signal.hpp`) are the serialization seam; the
[cluster](cluster.md) transport only adds/strips the `ZETTA/1 SIGNAL ` framing.

Every kind may additionally carry an optional **`traceparent`** (W3C trace
context, [observability.md](../architecture/observability.md), #640): set it on
a `Signal` before sending and the receiving node's handling can record spans
that join the SENDER's trace — one trace per distributed operation. Omitted
entirely when the sender is not in a trace; the steal offers ride it through
the work-stealing protocol (`schemas/steal_offer.schema.json`).

## Where it sits

```
include/zetta/cluster/signal.hpp   Signal / SignalKind / SignalResponse / SignalHandler;
                           encode_signal / decode_signal (the envelope seam)
src/core/cluster/signal.cpp        the envelope JSON form + its JSON-Schema boundary
src/core/cluster/cluster.cpp       the transport: a per-peer outbox, the per-link writer,
                           RPC correlation, and signal dispatch on the read pump
src/core/base64.hpp        shared base64 (the signal payload + the gossip blobs)
schemas/signal_envelope.schema.json   the draft-07 schema (mirror)
```

The `zetta::cluster::Cluster` API (`set_signal_handler`, `request`, `send_event`,
`broadcast_event`, `send_task`) is the public surface; it exposes no backend
types, like the rest of the cluster.

## Limits / future

- **Best-effort, at-most-once.** No retries, acks, or persistence for events/
  tasks; a signal to an offline peer is dropped. Reliable job delivery is the job
  coordinator's concern, built on top.
- **Handlers run on the network thread.** Heavy compute must be offloaded — the
  [work-stealing scheduler](../compute/work-stealing-scheduler.md) is built on this seam and
  does exactly that: its handlers only move task descriptors, and the compute runs
  on its own pool, off the loop.
- **Latency.** The outbox is drained on a short poll rather than an explicit
  cross-thread wake (the backend's frame allocator isn't thread-safe). If
  latency-sensitive [collectives](collectives.md) need it, a self-pipe wake is the
  natural next step.
- **Plaintext.** Same as the rest of the transport — TLS isn't wired yet.

## Consumers (event/RPC verbs)

The signal plane is the substrate for several layers, each using its own `method`
verbs over the same envelope: `collective.contribute` / `collective.result`
([collectives](collectives.md)), `steal` / `steal_result`
([work-stealing scheduler](../compute/work-stealing-scheduler.md)), `ddf.shuffle`
([distributed sort](../ddf/ddf-distributed-sort.md)), `fulltext.query`
([distributed full-text](../indexing/distributed-fulltext.md)), and the fire-and-forget
**`node.metrics`** event the [per-node telemetry exchange](../gui/cluster-node-gauges.md)
broadcasts (#584) so the headed cluster view can animate every node's resource
gauges. Each rides a schema-validated payload (the
[collective message](collectives.md) / telemetry boundary pattern); none adds a new
wire verb beyond `ZETTA/1 SIGNAL`.

## See also

- [collectives.md](collectives.md) — allreduce / broadcast / barrier / gather
  built on this layer, the first consumer of the signal plane.
- [cluster.md](cluster.md) — the equal-peer transport this rides on.
- [work-stealing-scheduler.md](../compute/work-stealing-scheduler.md) — the cross-node steal
  protocol built on this layer (the first real consumer).
- [cluster-node-gauges.md](../gui/cluster-node-gauges.md) — the ephemeral `node.metrics`
  telemetry exchange that animates the cluster view's per-node gauges (#584).
- [replication.md](replication.md) — the *replicated* (non-ephemeral) state plane,
  for contrast.
