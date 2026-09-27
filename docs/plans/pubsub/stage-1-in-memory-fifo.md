# Stage 1 — an in-memory topic

**Status:** draft, revision 21 (after [D1–D31](decisions.md)). No partitions:
a topic is one ordered store ([D13](decisions.md)). Settled parts cite
their decision; everything else is a proposal waiting on
[`questions.md`](questions.md).

## Shape

```
publish ──► [ topic ]
              a mutex + condition_variable per queue  (D1, D5)
              event_store  ◄── pluggable               (D2)
                 default: TopicRingStack = std::deque of ring buffers (D21)
                 alt:     std::queue (std::deque)
              subscriptions: each has a cursor + delivery options (D3)
              message heap: events too big for a slot   (D4)
```

## Events (D4)

An event has an **envelope** ([D26–D29](decisions.md)): `from` (stamped by the
node from the authenticated principal), `to`, `cc`, `bcc` and `reply-to` (lists
of `kind:name` addresses, wildcards allowed, e.g. `agent:security-*`), a
`correlation-id`, and a `topics-chain` (the topics it passed through, used to
stop loops). The envelope is variable-length, so
it is stored with the payload (inline or on the heap), not in the fixed header;
`bcc` is removed from every copy except the bcc recipient's own.

A slot in the ring holds a fixed-size header plus a small inline area:

```cpp
struct event_header {
    std::uint64_t sequence;   // assigned by the topic, monotonic
    std::uint32_t size;       // payload bytes
    encoding      kind;       // blob, json, ... (enum, extensible)
    std::uint16_t mime;       // interned MIME type id (blob); 0 = none
    std::uint8_t  priority;   // D3, if priority is per message
    std::uint8_t  flags;      // inline | heap
    // reserved: partition key (D13; unused while topics are unpartitioned)
};
// payload: inline bytes when size <= inline_capacity, else an event_ptr
// to an immutable, refcounted event on the message heap.
```

Proposals here, each a question below: MIME types are **interned** to a 16-bit
id so a header stays small ([Q15](questions.md)); a heap event is **immutable
and refcounted**, so broadcast never copies it ([Q14](questions.md)).

## Storage (D2)

```cpp
class event_store {                       // runtime-chosen, so virtual
public:
    virtual ~event_store() = default;
    virtual std::uint64_t append(event) = 0;           // returns the sequence
    virtual const event* at(std::uint64_t seq) const = 0;
    virtual void release_before(std::uint64_t seq) = 0; // min cursor advanced
};
```

- **`TopicRingStack`** (default, [D21](decisions.md)): a `std::deque` holding
  fixed-capacity ring buffers (`std::vector<slot>`). The stack's mutex protects
  changes to the stack (pushing and removing rings); whether it also guards
  appends and reads is [Q25](questions.md). Full → push a new ring (from a small free list if
  one is spare). A ring is returned to the free list when every entitled cursor
  has passed its last sequence.
- **`std::queue`-backed** (alternative, D2): `std::queue` only has
  `front`/`pop`, so it feeds one reader; suitable for work-queue topics, not
  broadcast.

## Subscribing (D19)

By default a subscriber joins the topic's subscriber list and receives events
published after it joined, as its delivery rules define. Its start position is
configurable at every level (oldest held, a sequence number).

## Queues, locks and waiting (D1, D5, D21, D23)

No coroutines in stage 1; subscribers are threads. **Every queue has its own
mutex and condition variables**; the `TopicRingStack`'s mutex only guards adding
and removing rings (lock order: stack, then queue).

A topic may be a **chain of queues**, chosen by configuration:

```
producers ──► producer queue ──► internal queue (TopicRingStack) ──► consumer queue per subscriber
```

Each hop has one writer side and one reader side. Payloads move as refcounted
event pointers, never copied per hop. The simplest configuration is the
internal queue alone, with subscribers reading it directly.

**Dispatch** ([D24](decisions.md)): a dispatcher thread per topic by default; a
shared pool (sized to the detected CPUs) once topics exceed a threshold (proposal
2 × CPUs, with hysteresis) or a heuristic fires; inline (the publisher's thread
does the hops) by configuration. Topics can be pinned to a mode. Every switch is
logged.

## Delivery (D3)

The same options object can be set on the topic (default), on a subscription,
and on a single publish ([D6](decisions.md)). The request is layered most specific
first, then bounded by governance for that principal on that topic
([D7](decisions.md), [`governance.md`](governance.md)); stage 1 takes an injected
authorization interface. A request over its bounds is clamped by default or
rejected, per topic ([D8](decisions.md)); `publish` returns the effective options.

```cpp
struct delivery {
    recipients who;        // all | one | n(k)
    ordering   order;      // fifo | priority (reordering allowed)
    std::uint8_t priority; // D9
    guarantee  delivery;   // at_most_once | at_least_once | exactly_once (D9)
};
```

How this maps onto cursors:

- **all** (broadcast): each subscriber has its own cursor over the shared store.
- **one** (work queue): subscribers in a group share one cursor; each event is
  claimed once.
- Because a publish can name its own recipients (D6), each event carries its
  own delivery state; per-consumer copies alone are not enough.
- **n(k)**: each event carries a remaining-claims count, decremented under the
  lock; the event is done at 0. Nobody surveyed ships this ([research/02](research/02-growable-rings-and-delivery.md)).
- **priority**: one ring per priority level, the reader takes the highest
  non-empty level; needs aging or low priority can starve.

## Guarantees (D9)

- **at-most-once:** a delivered event is done for that recipient.
- **at-least-once:** a delivery is *in flight* until the subscriber calls
  `ack(sequence)`; past its deadline it is redelivered. The event's storage is
  released only when every entitled recipient has acknowledged it.
- **exactly-once:** at-least-once plus a per-subscription set of processed event
  ids, so a redelivery is dropped before the subscriber sees it. In-memory only
  until stage 3.

## Limits and overflow (D10)

Settable at each level, bounded by governance:

| Level | Setting |
|---|---|
| topic | memory cap (bytes), disk cap (bytes), overflow policy, spill allowed |
| subscription | how far it may fall behind (events or bytes), its own overflow policy |
| message | time to live; whether it may be dropped under pressure |

Overflow policies: `unbounded` · `skip_ahead` (the slow subscriber jumps forward
and receives a gap notice with the count it missed) · `disconnect` ·
`hold_publisher` (with a timeout, then fail) · `reject_new` · `spill_to_disk`.

**Defaults are detected, not guessed** ([D11](decisions.md),
[research/04](research/04-detecting-resource-budgets.md)). Every limit is in
bytes, never an event count. (Two rounds of fixed defaults were rejected: 64 MiB
/ 10,000 events was far too small for a topic that sees more than 10k
events/s, and a fixed throughput target is still a guess.)

**Detection order for memory** (the first that answers wins, then the tightest):

1. configuration (always wins, and is logged as such);
2. the cgroup limit: v2 `memory.max`, or v1 `memory.limit_in_bytes`, taking the
   tightest ancestor;
3. the Kubernetes request/limit from Downward API environment variables, if the
   operator sets them;
4. the host: `hw.memsize` (macOS) or `MemTotal` (Linux), with current use from
   `host_statistics64` / `MemAvailable`.

Disk comes from `statvfs` on the data directory (and an `ephemeral-storage`
limit if exposed). CPU from `cpu.max` / `cfs_quota_us` and the cpuset.

**Derived defaults** (fractions of the detected budget; the fractions are
proposals): pub/sub memory for the node = 25% of the memory budget, shared by
every topic on it; a subscription may lag as far as its topic holds;
spill disk = 50% of the data directory's free space; hold publisher 5 s, then
fail.

**The log line**, once at startup and again if a limit changes:

```
pubsub: memory budget 4.0 GiB (cgroup v2 memory.max /kubepods/.../zetta-0; host 256 GiB ignored)
pubsub: memory for topics 1.0 GiB (25% of budget; override: pubsub.memory)
pubsub: spill disk 180 GiB (50% of 360 GiB free on /var/lib/zetta; override: pubsub.spill_disk)
pubsub: cpu 1.5 (cgroup v2 cpu.max 150000/100000; cpuset 0-15)
```

When full: at-most-once → `skip_ahead`; at-least-once and exactly-once →
`spill_to_disk`, then `hold_publisher`, then fail.

**Disk tier:** a full memory segment is written whole to a file under the node's
data dir and dropped from memory; the ring's segment list records where each
segment lives. Readers find a segment by sequence number, so a cursor never
cares which tier it is in. Spilled segments are reloaded on start
(D17); ephemeral topics never spill. Spilling is not a durability guarantee
([research/03](research/03-memory-limits-and-spill.md)).

**Spilling is logged** per topic ([D12](decisions.md)): on the first spill, every 30 s
while it continues, and when it ends.

```
pubsub: WARN orders spilling to disk: memory 1.0 GiB full; slowest subscriber billing-sync at seq 88121004 (lag 1.0 GiB); expect higher latency
pubsub: WARN orders still spilling (2m30s): 6.2 GiB on disk, +41 MiB/s; reading from disk: billing-sync (lag 6.9 GiB)
pubsub: INFO orders no longer spilling after 4m10s; 9.8 GiB spilled in total
```

## Topics as objects (D14–D16)

- **Defined at runtime:** `CREATE TOPIC orders WITH (persistent = true, ...)`
  over SQL, or `create_topic` over MCP/REST; `DROP TOPIC`; listed in
  `system.pubsub_topics`; governed by `pubsub:manage`.
- **Definitions are replicated state** (a CRDT key, like tables and roles), so
  they are persisted and reloaded after any shutdown and visible on every node.
- **Default topics** (D17, D18) keep events in memory, write them to disk on
  graceful shutdown, spill on overflow, and reload on start. Crash recovery is
  best effort (memory-mapped segments are one cheap way, research/06). Start-up
  logs what was recovered and what was lost:
  `pubsub: INFO orders recovered 1,204,339 events (seq 88121004..89325342); 0 torn`.
- **Ephemeral topics** (non-default): never touch disk; events discarded on
  shutdown.
- **Durable topics** (fsync guarantees, replay): later.
- **Auto-creation** is a pub/sub configuration setting, per name prefix;
  governed by `pubsub:create`; logged and audited with the defaults applied.

## Shutdown (D20)

SIGTERM (what Kubernetes sends) or SIGINT sets zetta's stop flag. Pub/sub then
stops accepting publishes, writes each topic's held events to disk within a
budget (default proposal 25 s, under Kubernetes' 30 s default grace period;
configurable), and logs:

```
pubsub: INFO shutdown: flushing 14 topics, 3.1 GiB held; budget 25s
pubsub: INFO shutdown: flushed 14/14 topics in 11.4s
pubsub: WARN shutdown: budget spent; orders lost 212,004 events (seq 90011002..90223005)
```

## Exposure (D22)

Internal by default. A topic can be exposed over MCP/REST by configuration:
`publish`, `subscribe` (events streamed over SSE), and `create_topic` /
`drop_topic`, governed like any capability.

## Inboxes and wildcards (D28, D30, D31)

- Every addressable entity has an **inbox** (a consumer queue, D23).
- A wildcard address is resolved through a **cache**: computed on first use,
  refreshed only when the subscriber list changes, bounded in size.
- An inbox outlives its subscriber's disconnect for a retention the subscriber
  chooses: **24 hours by default for agents**, or not at all. A waiting inbox
  counts against limits and spills like any queue.
