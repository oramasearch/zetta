# Stage 1 — an in-memory topic

**Status:** draft, revision 14 (after [D1–D17](decisions.md)). No partitions:
a topic is one ordered store ([D13](decisions.md)). Settled parts cite
their decision; everything else is a proposal waiting on
[`questions.md`](questions.md).

## Shape

```
publish ──► [ topic ]
              a mutex + condition_variable per queue  (D1, D5)
              event_store  ◄── pluggable               (D2)
                 default: segmented_ring = deque of vector rings
                 alt:     std::queue (std::deque)
              subscriptions: each has a cursor + delivery options (D3)
              message heap: events too big for a slot   (D4)
```

## Events (D4)

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

- **`segmented_ring`** (default): a `std::deque` of fixed-capacity
  `std::vector<slot>` rings. Full → push a new ring (from a small free list if
  one is spare). A ring is returned to the free list when every entitled cursor
  has passed its last sequence.
- **`std::queue`-backed**: a finding worth your eye — `std::queue` only exposes
  `front`/`pop`, so it can serve **one** reader (work queue), not broadcast with
  independent cursors. A `std::deque` used directly can, because it is indexable.
  See [Q12](questions.md).

## Subscribers and waiting (D1, D5)

No coroutines in stage 1. Subscribers are threads. **Each queue carries its own
`std::mutex` and `std::condition_variable`**: the shared ring (or deque) has
one, and each per-consumer queue, if the delivery mode uses them, has its own.
`publish` appends under the target queue's lock, releases it, then notifies.

Open: where the per-consumer queues exist at all depends on how delivery is
chosen ([Q11](questions.md)); if a publish touches several queues, the lock
order goes here.

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
- **Default topics** (D17) keep events in memory, write them to disk on
  graceful shutdown, recover them after a crash (proposal: memory-mapped
  segments, [Q24](questions.md)), spill on overflow, and reload all of it on
  start.
- **Ephemeral topics** (non-default): never touch disk; events discarded on
  shutdown.
- **Durable topics** (fsync guarantees, replay): later.
- **Auto-creation** is a pub/sub configuration setting, per name prefix;
  governed by `pubsub:create`; logged and audited with the defaults applied.
