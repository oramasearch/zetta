# Decisions

Each entry: what, why, what it costs, what would make us revisit. Settled by Steve
unless noted.

## The standing rule (from D6, D9, D10, D19)

**Every pub/sub setting is configurable at every level (topic, subscription,
message, where it makes sense), bounded by governance (D7, clamped by default,
D8), with a sane default detected or chosen for the common case (D11), and
logged when it matters.** A new setting follows this rule without asking.

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

## D6 — Delivery semantics can be set at any level: topic, subscription, message (2026-09-27)

**What.** Recipients, ordering and QoS/priority may each be set on the topic
(its default), on a subscription, and on a single publish, at one level or
several at once. Answers [Q11](questions.md).

**Why.** Refines D3: the flexibility is the point, and fixing the level would
recreate the Kafka limitation we are avoiding.

**Cost.** When two levels disagree there must be one written rule for which
wins ([Q17](questions.md)). And because a single publish can name its own
recipients, the store cannot rely only on per-consumer copies decided at
subscribe time: each event needs its own delivery state (e.g. a remaining-claims
count), checked under its queue's lock.

**Revisit if** the combination rules become too hard to explain in one table.

## D7 — Governance decides what each level may ask for (2026-09-27)

**What.** When topic, subscription and message settings disagree, the outcome is
decided by the security and governance rules on the topic, the user and the
subscriber, through zetta's **central governance model** (users, roles,
permissions, scoped grants, policies, audit), not by a fixed precedence built
into pub/sub. Answers [Q17](questions.md). Detail: [`governance.md`](governance.md).

**Why.** One model already governs tables, columns, blobs and MCP tools; topics
join it rather than inventing a second one. It is replicated state, so it works
cluster-wide in stage 2 without new machinery.

**Cost.** Pub/sub needs a new resource kind (`Topic`), new permission atoms and
policies over delivery options; every publish and subscribe pays an
authorization check (cacheable per subscription). What happens when a request
exceeds its bounds is [Q18](questions.md).

## D8 — Over-bound requests: clamp by default, reject if the topic says so (2026-09-27)

**What.** Each topic has a setting for requests that exceed their governance
bounds: **`clamp`** (the default) lowers the request to the bound and delivers;
**`reject`** fails it with an error naming the rule. Both outcomes are audited.
Answers [Q18](questions.md).

**Why.** Clamping keeps producers working when a bound tightens; topics where
silently lowering a request would be wrong can opt into rejection.

**Cost.** Clamp as the default departs from zetta's deny-by-default posture
elsewhere, so a clamp must never be silent: `publish` and `subscribe` return the
**effective** options they were granted, and the audit event records requested
vs effective. Clamping applies only to *delivery options*; a principal with no
`pubsub:publish` / `pubsub:subscribe` grant on the topic is still denied outright.
Changing the setting is itself governed (`pubsub:manage`).

## D9 — QoS is priority and delivery guarantee, both configurable at every level (2026-09-27)

**What.** QoS is two settings, not one:
- **priority**: which event is delivered first;
- **guarantee**: at-most-once, at-least-once (acknowledge, redeliver when not
  acknowledged), exactly-once.

Each can be set on the topic, the subscription and the message, and is bounded
by governance like every other delivery option ([D6](#d6--delivery-semantics-can-be-set-at-any-level-topic-subscription-message-2026-09-27),
[D7](#d7--governance-decides-what-each-level-may-ask-for-2026-09-27),
[D8](#d8--over-bound-requests-clamp-by-default-reject-if-the-topic-says-so-2026-09-27)).
Answers [Q16](questions.md).

**Why.** Same principle as D6: the caller chooses at runtime.

**Cost.**
- **At-least-once** needs acknowledgements, a redelivery deadline per delivered
  event, and holding the event until every entitled recipient has acknowledged
  it. Unacknowledged events pin ring segments, so it sharpens the growth
  question ([Q13](questions.md)). Subscribers must tolerate duplicates.
- **Exactly-once** in memory means at-least-once plus de-duplication: a stable
  event id and a per-subscription record of ids already processed. It is only
  exact within one process lifetime until stage 3 makes it durable.
- **Priority** breaks FIFO for events of different priority, and strict priority
  starves low priority unless waiting events age upward.

## D10 — Memory limits and overflow are configurable at every level, with defaults; disk is an overflow tier (2026-09-27)

**What.** How much a topic may hold, and what happens when it is full, is set at
the topic, subscription and message levels (bounded by governance, like every
option), with sane defaults. Overflow policies include spilling ring segments to
**disk**. Answers [Q13](questions.md). The proposed defaults are
[Q19](questions.md).

**Why.** A stuck subscriber must not take the process down, and no single
policy fits every topic (research/03: RabbitMQ, NATS, Redis and Pulsar each
default differently).

**Cost.** Disk is a second tier with its own cap and policy, not an unlimited
extension (research/03). A subscriber reading spilled segments hits a latency
cliff. Spilling is not durability: without `fsync` a crash loses spilled events,
so durable topics stay a stage 3 decision. Cursors are sequence numbers so they
stay valid across tiers.

## D11 — Defaults are detected from the environment, logged, and overridable (2026-09-27)

**What.** Limits are not guessed constants. At startup the node detects its
budget (cgroup memory and CPU limits, Kubernetes requests and limits exposed to
the pod, host memory total and used, free disk on the data directory) and derives
pub/sub's defaults from it. Configuration overrides any detected value. **Every
detected input, where it came from, and the value chosen is written to the log.**
Replaces the fixed defaults proposed under Q19/Q20.

**Why.** A fixed number is wrong on most machines: too small on a 256 GiB host,
too large in a 2 GiB pod. The JVM, Go and .NET all size from the container limit
for the same reason (research/04).

**Cost.** Zetta's `node_metrics.cpp` reads only host figures today; inside a pod
that is the host's memory, not the pod's. Detection needs cgroup v1 and v2
readers (with the tightest-ancestor rule) and, for requests, the Downward API
environment variables, which the operator must set on the Zetta pod. Detection
can fail or mislead, hence the log line.

**Revisit if** the detected budget turns out to be the wrong basis (e.g. several
heavy subsystems each claiming a share of the same budget need one allocator).

## D12 — Spilling to disk is logged when it starts, repeatedly while it lasts, and when it ends (2026-09-27)

**What.** Spilling will certainly hurt performance, so it is never quiet:
- a **warning when a partition first spills**, naming the topic, partition,
  the memory limit it hit, and the subscriber holding it back;
- **repeated warnings while it keeps spilling**, rate-limited (proposal: every
  30 s per partition), with bytes on disk, the spill rate, and which subscribers
  are reading from disk and how far behind they are;
- a line **when it stops** (the partition is back in memory), with the total
  spilled and the duration.

**Why.** An operator seeing a latency cliff should find the cause in the log
without having to guess; a single line at the start is easy to miss in a long
spill.

**Cost.** Log volume, hence the rate limit. Proposal (zetta RULE-7): the same
state is also a read view (`system.pubsub_topics`: memory, disk, spilling
since, slowest subscriber) so it can be queried rather than only grepped.

## D13 — No partitions now; nothing may rule them out later (2026-09-27)

**What.** Stage 1 topics are not partitioned: a topic is one ordered store. We
may add partitions later, so the design must not preclude them. Answers
[Q21](questions.md).

**Why.** Keep stage 1 small; partitions are the likely route to scaling a topic
across cores and nodes (stage 2), so the door stays open.

**How the door stays open (proposal):**
- sequence numbers are per topic today and become per partition later, so no
  API promises a total order beyond "within this topic's store";
- the event header keeps room for a **partition key** (unused now);
- limits, logs and read views are written per topic now and gain a partition
  column later, without renaming (`system.pubsub_topics`);
- a subscription names a topic, never a store, so a later partitioned topic can
  hand it several stores.

**Cost.** Callers may come to rely on one total order per topic; the API docs
must say that order is only promised per store.

## D14 — Topics are runtime objects, listed like tables, and survive restarts (2026-09-27)

**What.** Topics are created at runtime (`CREATE TOPIC` over SQL, a
`create_topic` capability over MCP/REST), listed in `system.pubsub_topics`,
governed by `pubsub:manage`, and their definitions are **persisted to disk and
reloaded** after any shutdown, graceful or not. Like every other zetta entity, a
topic definition is replicated state (a CRDT key), which gives both persistence
and cluster-wide visibility. Answers [Q22](questions.md).

**Interpretation corrected by Steve (D17):** events survive restarts by
default too, not only on persistent topics.

## D15 — Persistent topics keep their events across graceful shutdown and crash (2026-09-27) — *narrowed by D17*

**What.** A topic can be declared **persistent**. Its events are written to
append-only segment files with an index and a checkpoint, and are reloaded after
a restart, including after a crash (research/05). Non-persistent topics keep
events only in memory (and spill files, deleted on startup, D10).

**Why.** Some topics are worth their disk cost (at-least-once across restarts,
replay), most are not; the caller chooses.

**Cost.** This pulls durability forward from stage 3 into the design now. For a
persistent topic the disk tier *is* the log, so spilling and persistence become
one mechanism. A crash loses whatever was not yet `fsync`ed; that window is the
fsync policy ([Q23](questions.md)). Recovery after a crash scans and repairs the
tail past the last checkpoint.

## D16 — Topics can be auto-created from pub/sub configuration, like Kafka (2026-09-27)

**What.** Besides explicitly defined topics, the pub/sub configuration can allow
**implicit creation**: publishing or subscribing to a topic that does not exist
creates it with configured defaults.

**Why.** Convenient for development and for engine-internal topics.

**Cost (research/05):** Kafka users often turn this off because a typo creates
a topic that silently inherits generic defaults. Proposals: auto-creation is
still governed (the caller needs `pubsub:create` on the name or prefix, as Kafka
requires `CREATE`); it can be enabled per name prefix (like Pulsar's
per-namespace override) rather than only all-or-nothing; every auto-creation is
logged and audited with the defaults it received; and auto-created topics are
non-persistent unless configuration says otherwise.

## D17 — By default events survive restarts, crashes and overflow; memory-only is opt-in; durable topics come later (2026-09-27)

**What.** The **default** topic is memory-first, but its events are not lost:
- on a **graceful shutdown** they are written to disk and reloaded on start;
- on a **crash** they are recovered on start (mechanism: [Q24](questions.md));
- on **overflow** they spill to disk (D10), and spilled segments are **reloaded**
  on start, no longer deleted.

A **non-default** setting makes a topic **ephemeral**: it never uses disk, its
events are discarded on shutdown, and `spill_to_disk` is not available to it.
Fully **durable** topics (an fsync guarantee on every publish, replay as a log)
are deferred. Supersedes the event half of D15 and the "deleted on startup" rule
of D10. Answers [Q23](questions.md).

**Why.** Losing events because a node restarted is the surprising outcome, so it
must be the one you ask for.

**Cost.** Surviving a crash without writing every event to disk needs the ring
segments to live somewhere the process's death does not erase (research/06).
Recovery must handle a torn last event (framing + checksum). The node needs a
data directory with room for the whole in-memory contents at shutdown.

## D18 — Restart and crash survival is best effort and logged; persistence is designed later (2026-09-27)

**What.** For stage 1, surviving restarts and crashes (D17) is **best effort**:
write what we can on shutdown, recover what we can on start, and **log what was
saved, recovered and lost**. No fsync policy, no machine-failure guarantee.
Answers [Q24](questions.md). The real guarantees are part of designing
persistent topics, later.

**Why.** Keep stage 1 moving; persistence deserves its own design.

**Cost.** None hidden: the log says exactly what survived.

## D19 — A new subscriber joins and receives from then on, by default (2026-09-27)

**What.** By default a new consumer simply joins the topic's subscriber list and
starts receiving events published after it joined, as its delivery rules
define. Where it starts is configurable at every level (the standing rule):
e.g. the oldest event still held, or a given sequence number. Answers
[Q7](questions.md).

## D20 — Shutdown flush runs from the signal handler and aims to finish in under 30 s (2026-09-27)

**What.** Zetta runs in Kubernetes, which sends **SIGTERM** on pod shutdown and
**SIGKILL** after `terminationGracePeriodSeconds` (Kubernetes default 30 s).
Zetta already handles SIGINT and SIGTERM (`src/core/signals.cpp` sets a stop
flag the run loops poll). On that signal, pub/sub stops accepting publishes and
writes topics to disk (D17), with a **budget that finishes under 30 s by
default** (proposal: 25 s, leaving margin before SIGKILL), configurable. It logs
start, progress and outcome; whatever does not fit in the budget is logged as
lost (D18).

**Why.** A flush that outlives the grace period is killed halfway; better to
finish within it and say what was left.

**Cost.** Large in-memory topics may not fit in 25 s of disk bandwidth; the
operator sets 60 s for stores today (orama-operator `docs/components.md`), so
the budget could be read from configuration the operator sets. The handler only
sets a flag (async-signal-safe); the flush runs on the node's shutdown path, not
inside the handler.

## D21 — The default store is a `TopicRingStack`: a deque of ring buffers under one mutex (2026-09-27)

**What.** The default store (D2) is named **`TopicRingStack`**. It is a
`std::deque` that **holds ring buffers**; the deque is not an alternative store.
The `TopicRingStack`'s **mutex protects changes to the stack**: pushing a new
ring when the newest is full, and removing a ring every subscriber has passed.
Corrects the Q12 settlement, which had treated the deque as a second store type.

**Open:** whether appends and reads inside a ring take the same mutex or the
ring's own ([Q25](questions.md)).

## D22 — Exposure over MCP/REST is optional, off by default (2026-09-27)

**What.** A topic can optionally be exposed externally (MCP/REST: publish,
subscribe streaming over SSE, create/drop). **By default it is not exposed**:
pub/sub is an engine-internal facility unless configuration says otherwise, and
exposure is governed (D7). Answers [Q8](questions.md).

**Cost.** Departs from zetta's "every capability ships on the API and MCP" rule
for the default; the capabilities still exist in the registry, gated by the
per-topic exposure setting, so the rule is kept and the default is off.

## D23 — Each queue has its own lock and condition variables; a topic may chain several queues (2026-09-27)

**What.** Every queue (each ring buffer in the `TopicRingStack`, a consumer
queue, …) has **its own mutex and condition variables**. The stack's mutex only
guards adding and removing rings (D21). Lock order: stack before queue. To
simplify locking, a topic may be built from **several queues in a chain**, chosen
by configuration, e.g.:

```
producers ──► producer queue ──► internal queue (TopicRingStack) ──► consumer queue per subscriber
```

Each hop has one side writing and one side reading, so producers contend only
with producers, and each consumer only with the dispatcher feeding it. Answers
[Q25](questions.md).

**Cost.** Each hop adds latency and a handoff; payloads must not be copied per
hop (refcounted event pointers, D4). Something has to move events along the
chain ([Q26](questions.md)).

## D24 — Dispatch: a thread per topic by default, a shared pool past a threshold, inline as an option (2026-09-27)

**What.** Configurable. **Default: a dispatcher thread per topic.** When the
topic count passes a threshold, or a heuristic fires, the node **moves to a
shared pool** of dispatcher threads. **Inline dispatch** (the publisher's thread
carries its event through every queue, no dispatcher) is a configuration option.
Answers [Q26](questions.md).

**Proposals under the standing rule** (overrulable):
- the threshold is derived from the detected CPU budget (D11): switch when
  topics exceed **2 × detected CPUs**; the pool size is the detected CPU count;
- heuristics that also trigger the switch: total pub/sub threads over a limit,
  or most dispatcher threads idle most of the time;
- **hysteresis**: switch back to per-topic threads only when the count falls
  well below the threshold (e.g. half), so the node does not flap;
- a topic can be **pinned** to a dedicated thread (or to inline) by
  configuration, and keeps it in pool mode;
- the pool serves topics fairly: round robin, with a cap on events moved per
  turn;
- **every switch is logged** with the reason:
  `pubsub: INFO dispatch: 34 topics > threshold 32 (2 × 16 cpus); moving 32 topics to a pool of 16 threads`.

**Cost.** Two dispatch modes to test, and a migration step that must hand a
topic from its thread to the pool without losing or reordering events.

## D25 — Anything can publish or subscribe; agents first, including agent tasks (2026-09-27)

**What.** Pub/sub is a general engine facility: any part of zetta can publish or
subscribe. **Agents are the main user**, and **agent tasks** (handing work to
agents and getting results back) will run over pub/sub. The agent-task design
lives in a separate place; this design only has to serve it. Answers
[Q1](questions.md).

**What that implies here:** the patterns agent tasks need (work queues with one
recipient per task, at-least-once delivery with acknowledgement, priority,
request/reply) must be expressible with the options already decided, and the
stage 1 tests should include an agent-task-shaped workload.

**Open:** a link to the agent-task design once it has a home.

## D26 — Everything addressable has a unique typed name (2026-09-27)

**What.** Topics, agents (subscribers) and other entities each have a **unique
name of the form `kind:name`**: `topic:abc123`, `agent:405`, `table:logs`. These
are the addresses events are sent to and replies come back to.

**Why.** One address space for every participant, so an event can go to a
topic, straight to an agent, or to a table without separate APIs.

**Cost.** Pub/sub now routes to more than topics; what it means to deliver to a
non-topic address is [Q28](questions.md). The `kind` list should line up with
governance's `ResourceKind` (database, table, blob, tool, …) plus `topic` and
`agent`, so one name works for both routing and grants.

## D27 — Events carry an email-style envelope: to, cc, bcc, reply-to, correlation id; wildcards allowed (2026-09-27)

**What.** Every event may carry:

| Field | Meaning |
|---|---|
| `to` | list of addresses: primary recipients |
| `cc` | list of addresses: also receive it |
| `bcc` | list of addresses: receive it, **not shown** to the other recipients |
| `reply-to` | list of addresses replies should go to |
| `correlation-id` | ties a reply to its request |

Any address may be a **wildcard**: `agent:*`, `agent:security-*` (a prefix glob
within the name). Request/reply is built in this way rather than left to the
agent-task layer. Answers [Q27](questions.md).

**Cost (research/07):** a sender addressing a wildcard is unusual (NATS, MQTT and
RabbitMQ only allow wildcards on the subscribing side), so: sending to a
wildcard is a separately governed right; its fan-out is capped and logged; and
whether a wildcard is expanded at send time or also reaches future matches must
be defined. Bcc must be stripped from every copy except the bcc recipient's own.
The envelope never proves the sender; identity comes from governance.
