# Decisions

Each entry: what, why, what it costs, what would make us revisit. Settled by Steve
unless noted.

## D1 — Locks and condition variables are allowed inside pub/sub (2026-09-27)

**What.** The topic may use `std::mutex` and `std::condition_variable` internally.
Lock-freedom is not a goal.

**Why.** Simple and obviously correct beats clever for the first version; a
bounded critical section around a queue push/pop is cheap.

**Cost.** This departs from zetta's first principle ("never a blocking
mutex/condition-variable surface", zetta `CLAUDE.md`). See D5 for how subscribers
wait. The zetta `CLAUDE.md` rule needs an explicit exception for this component
before code lands.

**Revisit if** measurement shows lock contention on a hot topic, or coroutine
subscribers pin pool threads.

## D2 — Several queue types behind one interface; the default is a growing ring (2026-09-27)

**What.** Storage is pluggable: e.g. a `std::queue` (deque-backed) and a
`std::vector` used as a ring buffer. The **default is the ring**. When a ring
fills, we allocate another ring and chain it (a deque of rings), rather than
blocking or dropping.

**Why.** Different workloads want different storage; the ring is compact and
cache-friendly; growing by segments means a burst never blocks a publisher.

**Cost.** Growth is unbounded unless something caps it; a slow subscriber can pin
old segments in memory ([Q13](questions.md)).

**Revisit if** memory growth under slow subscribers becomes a real incident.

## D3 — Delivery semantics are chosen at runtime (2026-09-27)

**What.** Unlike Kafka, where a topic's semantics are fixed by its partitions and
consumer groups, a zetta topic lets the caller choose at runtime: how many
recipients get a message, whether reordering is allowed, and QoS/priority.

**Why.** One primitive serves broadcast, work queues and prioritized work
without a second system.

**Cost.** These interact: priority breaks FIFO, strict order causes head-of-line
blocking, and "deliver to one" needs a shared claim where broadcast needs
per-subscriber cursors (research/02). Where the choice is made (per topic, per
subscription, per message) is open ([Q11](questions.md)).

## D4 — Events are binary, tagged with an encoding (2026-09-27)

**What.** An event is bytes plus a type tag: e.g. a blob with a MIME type, JSON,
and more later. Small events live inline in the ring; an event too large for the
ring is allocated on a message heap and the ring holds an **event pointer** to it.

**Why.** Binary-level events cross threads, and later the network, without a
second format; inline storage avoids an allocation for the common small event.

**Cost.** Two storage paths; the heap path needs a lifetime rule when several
subscribers read one event ([Q14](questions.md)).

## D5 — No coroutines in stage 1; a mutex per queue (2026-09-27)

**What.** Stage 1 does not integrate with the coroutine runtime. Subscribers are
threads that wait on a `std::condition_variable`. Locking is fine-grained: **a
mutex per queue** (per ring, per deque, or per consumer queue), not one lock for
the whole topic. Answers [Q10](questions.md).

**Why.** Keeps the first slice a plain, testable concurrent data structure; a
lock per queue keeps publishers and consumers of different queues from
contending.

**Cost.** A subscriber waiting on a condition variable holds a thread, so a
coroutine caller inside zetta has to wait from a dedicated thread or through
`runtime::run_blocking`. Several locks mean a lock order must be written down
wherever an operation touches more than one queue (e.g. a broadcast publish that
appends to every consumer queue).

**Revisit if** zetta code needs to subscribe from coroutines at scale; a
coroutine waiter list can then be added behind the same API.
