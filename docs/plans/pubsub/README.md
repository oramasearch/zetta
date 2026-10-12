# Pub/sub in zetta — design

**Status:** stage 1 is built. The design remains the decision record for shipped
node-local pub/sub and later stages; D24 now records the shared, topic-sized
dispatch pool from [#1157](https://github.com/oramasearch/zetta/issues/1157).

**Who uses it.** Anything in zetta can publish or subscribe; **agents** are the
main user, and **agent tasks** will run over pub/sub (designed separately,
[D25](decisions.md)).

**How this folder works.** Each file is one subject. Questions go into
[`questions.md`](questions.md); settled answers move into the page they govern.
Shipped behavior is documented in [../../pubsub/](../../pubsub/README.md), while
this folder preserves the decisions and later-stage design.

## Pages

1. [`context.md`](context.md) — what zetta already has that pub/sub builds on
   (Capy `task`, executors, `strand`, `async_event`), and the rules it must obey.
2. [`research/01-in-memory-fifo.md`](research/01-in-memory-fifo.md) — how others
   build an in-memory, thread-safe, coroutine-aware queue and fan-out: channels,
   lock-free MPMC, the Disruptor ring, Kafka/NATS logs, P0260.
3. [`research/02-growable-rings-and-delivery.md`](research/02-growable-rings-and-delivery.md)
   — segmented rings and reclamation, inline vs heap events, and how brokers
   choose recipients, ordering, priority and QoS.
4. [`research/03-memory-limits-and-spill.md`](research/03-memory-limits-and-spill.md)
   — how RabbitMQ, NATS, Redis and Pulsar cap queues, their overflow policies and
   defaults, and the pitfalls of spilling to disk.
5. [`research/04-detecting-resource-budgets.md`](research/04-detecting-resource-budgets.md)
   — detecting memory, CPU and disk budgets from cgroups, the Downward API and the
   host, and how the JVM, Go and .NET size themselves.
6. [`research/05-auto-creation-and-durability.md`](research/05-auto-creation-and-durability.md)
   — Kafka/Pulsar auto-creation and its pitfalls; how Kafka, NATS, RabbitMQ and
   Redis survive shutdown and crash (segments, index, checkpoint, fsync).
7. [`research/06-mmap-crash-survival.md`](research/06-mmap-crash-survival.md)
   — what memory-mapped segments survive (process crash yes, machine failure no),
   cgroup page-cache charging, SIGBUS and other pitfalls.
8. [`research/07-addressing-and-wildcards.md`](research/07-addressing-and-wildcards.md)
   — NATS/MQTT/RabbitMQ/Kafka/Akka wildcards, email To/Cc/Bcc/Reply-To and
   threading, and the pitfalls of sender-side wildcards.
9. [`decisions.md`](decisions.md) — the standing rule (every setting at every level,
   governed, with detected defaults) and every settled decision (D1–D39).
10. [`stage-1-in-memory-fifo.md`](stage-1-in-memory-fifo.md) — the stage 1
   specification, consolidated from every decision; its status line carries the
   revision.
11. [`governance.md`](governance.md) — how topics join zetta's central governance
   model (a `Topic` resource kind, `pubsub:*` permissions, policies over delivery
   options, audit).
12. [`questions.md`](questions.md) — open questions, answered ones kept with their
   answers.

## Stages (proposed, not agreed)

| Stage | What | Status |
|---|---|---|
| 1 | In-memory pub/sub on one node: addressed events, inboxes, runtime topics, governed delivery options | **built**, PR [#1035](https://github.com/oramasearch/zetta/pull/1035); docs in [../../pubsub/](../../pubsub/README.md) |
| 2 | The same API across the cluster (topics, inboxes and subscribers on any node) | designed later (D39) |
| 3 | Retention / replay (a log with per-subscriber offsets); restart/crash survival is in stage 1 (D17); fsync-durable topics are here | not started |
| 4 | Change events from tables and datasets (CDC) as topics | not started |
