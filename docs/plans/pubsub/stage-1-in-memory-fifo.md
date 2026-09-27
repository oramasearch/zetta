# Stage 1 — in-memory pub/sub specification

**Status:** draft spec, revision 26 (2026-09-27): a consolidated rewrite of
revisions 1–22 over decisions [D1–D35](decisions.md). Every section cites the
decisions it comes from; anything marked *proposal* was filled in under the
[standing rule](decisions.md#the-standing-rule-from-d6-d9-d10-d19) and can be
overruled. Open items are listed at the end.

## 1. What stage 1 is

An engine-internal message facility in zetta (`namespace zetta::pubsub`): any
part of zetta can publish or subscribe, and **agents** are the main user,
including **agent tasks**, which are designed separately and run on top of this
(D25). Events are held **in memory**, overflow to disk, and survive restarts on
a best-effort basis (D17, D18). No partitions (D13), no coroutines (D5), no
durability guarantees (later, with persistent topics).

**The standing rule:** every setting is configurable at every level where it
makes sense (topic, subscription, message), bounded by governance, clamped by
default when a request exceeds its bounds, with a detected or common-case
default, and logged when it matters (D6, D7, D8, D11).

## 2. Addresses and the envelope

**Addresses (D26).** Everything addressable has a unique `kind:name`:
`topic:abc123`, `agent:405`, `table:logs`. The kinds line up with governance's
`ResourceKind` plus `topic` and `agent`, so one name serves routing and grants.

**Inboxes (D28, D35).** Every addressable entity has an **inbox**, a queue of its
own, drained according to its kind:

| Kind | Default drain |
|---|---|
| `topic:` | publish to the topic's subscribers, by their delivery rules |
| `agent:` | hold for the agent to read |
| `table:` | insert each event as a row; an event that does not fit is dead-lettered |
| `function:` / `procedure:` | call the function or stored procedure with the event |
| `service:` | deliver to the service |
| `model:` | run inference on the event |

*Proposal:* each drain implements one `InboxDrain` interface, registered by kind,
and is governed like the direct operation (`sql:write` to insert, `serving:read`
to infer). Where a drain's result goes is [Q33](questions.md).

**Envelope (D27, D29, D33).** Every event may carry:

| Field | Meaning |
|---|---|
| `from` | the principal the event is for (`user:steve`); stamped by the node from authenticated principals, never trusted from the caller (D33) |
| `sender` | who actually sent it when acting for `from`: a **stack** of every agent in the delegation chain (`agent:ask-orama`, then `agent:security-scan`, …); governance checks every link back to `from`; the audit records the stack; at most 10,000 deep, beyond which the event is dropped with an error logged (D33, D34) |
| `to` | primary recipients |
| `cc` | also receive it |
| `bcc` | receive it; removed from every copy except the bcc recipient's own |
| `reply-to` | where replies go |
| `correlation-id` | ties a reply to its request |
| `topics-chain` | stack of topics the event passed through, newest last |

`to`, `cc`, `bcc` and `reply-to` are lists of addresses, and any address may be a
**wildcard** (`agent:*`, `agent:security-*`, a prefix glob within the name).

**Wildcards (D30).** A wildcard's matches are computed the first time it is seen
and **cached**; the entry is refreshed only when the subscriber list changes.
So a wildcard reaches whoever matches at send time. *Proposal:* the cache is
bounded, and sending to a wildcard is a separately governed right with a capped,
logged fan-out.

**Loops (D29).** *Proposal:* an event is never delivered to a topic already on
its `topics-chain`, and the chain has a maximum depth (default 32). Either stop
is logged and dead-lettered.

## 3. Events

**Encodings (D4, Q15).** An event is binary with an encoding tag: `blob` (with a
MIME type), `json`, `text`, more later. *Proposal:* MIME types are interned to a
16-bit id per node.

**Storage of one event (D4, Q14).** A ring slot holds a fixed header and a small
inline area. The payload and the variable-length envelope live inline when they
fit; otherwise the slot holds an **event pointer** to an immutable, reference-
counted event on the message heap, so fan-out and queue hops never copy it.
*Proposal:* the inline threshold is a per-topic setting, default 256 bytes.

```cpp
struct EventHeader {
    std::uint64_t sequence;   // per topic, monotonic
    std::uint32_t size;       // payload + envelope bytes
    Encoding      encoding;   // blob | json | text | ...
    std::uint16_t mime;       // interned MIME id (blob); 0 = none
    std::uint8_t  priority;   // D9
    std::uint8_t  flags;      // inline | heap
    // reserved: partition key (D13; unused while topics are unpartitioned)
};
```

## 4. Topics

**Runtime objects (D14).** Created with `CREATE TOPIC` over SQL (or
`create_topic` when exposed), dropped with `DROP TOPIC`, listed in
`system.pubsub_topics`. A topic definition is **replicated state** (a CRDT key,
like tables and roles), so it is persisted, reloaded after any shutdown and
known on every node; its events stay on the node that holds them.

**Auto-creation (D16).** The pub/sub configuration can let a publish or
subscribe to a missing topic create it, *proposal:* per name prefix, governed by
`pubsub:create`, and logged and audited with the defaults it received.

**Lifecycles (D17, D18).**

| Kind | Events on graceful shutdown | After a crash | Overflow |
|---|---|---|---|
| **default** | written to disk, reloaded on start | recovered, best effort | spill to disk |
| **ephemeral** (opt-in) | discarded | lost | never touches disk |
| **durable** | — later, with persistent topics — | | |

Start-up logs what was recovered and what was lost:

```
pubsub: INFO orders recovered 1,204,339 events (seq 88121004..89325342); 0 torn
```

## 5. Stores and queues

**`TopicRingStack` (D2, D21).** The default store: a `std::deque` that holds
fixed-capacity ring buffers. When the newest ring is full, a new ring is pushed
(from a small free list when one is spare); a ring is removed once every
subscriber entitled to it has passed its last sequence. The stack's mutex
protects **changes to the stack**. The alternative store is `std::queue`-backed,
which feeds one reader and so suits work queues only.

**Locks (D1, D5, D23).** Every queue has **its own mutex and condition
variables**. Lock order: stack, then queue. Subscribers are threads; there are
no coroutines in stage 1.

**Queue chains (D23).** By configuration a topic may chain queues so each hop
has one writing side and one reading side:

```
producers ──► producer queue ──► internal queue (TopicRingStack) ──► inbox per subscriber
```

The simplest configuration is the internal queue alone.

**Dispatch (D24).** What moves events along the chain: a **dispatcher thread
per topic** by default; a **shared pool** once the topic count passes a
threshold or a heuristic fires; **inline** (the publisher's thread) by
configuration. *Proposals:* threshold 2 × detected CPUs, pool size = detected
CPUs, switch back at half the threshold, topics can be pinned to a mode, the
pool serves topics round robin with a per-turn cap. Every switch is logged:

```
pubsub: INFO dispatch: 34 topics > threshold 32 (2 × 16 cpus); moving 32 topics to a pool of 16 threads
```

## 6. Subscribing and delivery

**Joining (D19).** A subscriber joins the topic's subscriber list and receives
events published after it joined, as its rules define. Start position is
configurable (oldest held, a sequence number).

**Inbox retention (D31).** A subscriber chooses how long its inbox keeps
receiving after it disconnects: **24 hours by default for agents**, or not past
the disconnect. A waiting inbox counts against limits and spills like any queue.

**Delivery options (D3, D6, D9).** Set on the topic (default), the subscription,
or the single publish:

```cpp
struct Delivery {
    Recipients   recipients;  // all | one (a group shares the event) | n(k)
    Ordering     ordering;    // fifo | priority (reordering allowed)
    std::uint8_t priority;
    Guarantee    guarantee;   // at_most_once | at_least_once | exactly_once
};
```

- **all:** every subscriber reads the shared store at its own position.
- **one:** a group shares a position; each event is claimed once.
- **n(k):** each event carries a remaining-claims count, decremented under its
  queue's lock. Because a single publish can choose its recipients, delivery
  state lives on the event, not only on subscriptions.
- **priority:** *proposal:* one ring stack per priority level, highest non-empty
  first, with aging so low priority is not starved.

**Guarantees (D9).**
- **at-most-once:** delivered means done.
- **at-least-once:** in flight until `ack(sequence)`; redelivered after a
  deadline; the event is released when every entitled recipient has acked.
- **exactly-once:** at-least-once plus a per-subscription record of processed
  ids; exact only within one process lifetime.

**Resolution (D7, D8).** The request is layered most specific first (message,
then subscription, then topic), then bounded by governance for that principal
on that topic. Over the bounds: **clamp** (default) or **reject**, per topic.
`publish` and `subscribe` return the effective options.

**Dead letters (D32).** Undeliverable events go to `topic:dead-letter`
(configurable) with a reason: retention expired, time to live expired, no match,
loop or depth stop, subscriber skipped ahead. If that fails, drop and log.

## 7. Governance

Detail in [`governance.md`](governance.md). Topics join zetta's central model
(users, roles, permissions, scoped grants, policies, audit) (D7):
`Topic` as a resource kind; `pubsub:publish`, `pubsub:subscribe`,
`pubsub:create`, `pubsub:manage`; policies that bound delivery options per role;
separate rights for wildcard sends; every decision audited. Stage 1 takes an
injected authorization interface so the data structure does not depend on
`zetta::Node`.

## 8. Limits and overflow

**Settings (D10).**

| Level | Settings |
|---|---|
| topic | memory cap, disk cap, overflow policy, spill allowed |
| subscription | how far it may fall behind, its own overflow policy |
| message | time to live, droppable under pressure |

Policies: `unbounded`, `skip_ahead` (with a gap notice), `disconnect`,
`hold_publisher` (then fail), `reject_new`, `spill_to_disk`. Every limit is in
bytes, never an event count.

**Detected defaults (D11).** Memory budget, first that answers, tightest wins:
configuration → cgroup (`memory.max` / `memory.limit_in_bytes`, tightest
ancestor) → Kubernetes requests/limits via Downward API variables → host
(`hw.memsize` / `MemTotal`). Disk from `statvfs` on the data directory; CPU from
`cpu.max` / `cfs_quota_us` and the cpuset. *Proposals:* pub/sub memory = 25% of
the budget; spill disk = 50% of free space; hold publisher 5 s. When full:
at-most-once → `skip_ahead`; at-least-once and exactly-once → `spill_to_disk`,
then `hold_publisher`, then fail.

```
pubsub: memory budget 4.0 GiB (cgroup v2 memory.max /kubepods/.../zetta-0; host 256 GiB ignored)
pubsub: memory for topics 1.0 GiB (25% of budget; override: pubsub.memory)
pubsub: spill disk 180 GiB (50% of 360 GiB free on /var/lib/zetta; override: pubsub.spill_disk)
pubsub: cpu 1.5 (cgroup v2 cpu.max 150000/100000; cpuset 0-15)
```

**Spilling (D10, D12).** A full ring is written whole to a file in the data
directory; positions are sequence numbers, so a reader does not care which tier
a ring is in. Spilling is logged on start, every 30 s while it lasts, and on end:

```
pubsub: WARN orders spilling to disk: memory 1.0 GiB full; slowest subscriber billing-sync at seq 88121004 (lag 1.0 GiB); expect higher latency
pubsub: WARN orders still spilling (2m30s): 6.2 GiB on disk, +41 MiB/s; reading from disk: billing-sync (lag 6.9 GiB)
pubsub: INFO orders no longer spilling after 4m10s; 9.8 GiB spilled in total
```

## 9. Shutdown (D20)

SIGTERM (what Kubernetes sends) or SIGINT sets zetta's stop flag
(`src/core/signals.cpp`). Pub/sub stops accepting publishes and writes held
events to disk within a budget, *proposal* 25 s, under Kubernetes' 30 s default
grace period, configurable:

```
pubsub: INFO shutdown: flushing 14 topics, 3.1 GiB held; budget 25s
pubsub: INFO shutdown: flushed 14/14 topics in 11.4s
pubsub: WARN shutdown: budget spent; orders lost 212,004 events (seq 90011002..90223005)
```

## 10. Exposure (D22)

Internal by default. By configuration, a topic's capabilities are exposed over
MCP/REST: `publish`, `subscribe` (streamed over SSE), `create_topic`,
`drop_topic`, governed like any capability. They are registered in the
capability registry either way, gated by the exposure setting.

## 11. Observability

*Proposal*, per zetta's observability rule: publish, subscribe, ack and
dead-letter open spans; `system.pubsub_topics` shows per topic memory held, disk
held, spilling since, subscriber count, slowest subscriber and its lag, and
dispatch mode.

## 12. Open

- Where a drain's result goes ([Q33](questions.md)).
- The link to the agent-task design, once it has a home (D25).
- The proposals marked above, until confirmed.
