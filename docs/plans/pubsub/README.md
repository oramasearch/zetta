# Pub/sub in zetta — design

**Status:** design in progress, started 2026-09-27. No code yet. There is no
pub/sub in zetta today and no issue for it (the only related issue is
[#1014](https://github.com/oramasearch/zetta/issues/1014), which reads Kafka
*as a source*; it does not make zetta a broker).

**How this folder works.** We design slowly, one decision at a time. Each file is
one subject. Questions go into [`questions.md`](questions.md) and are answered
there, with the date and who answered; a settled answer moves into the page it
governs. **One question at a time**; every design iteration is committed and
pushed to the design PR (branch `docs/pubsub-design`). Nothing here is built until the design says how it distributes (zetta's
first principle — see [`context.md`](context.md)).

## Pages

1. [`context.md`](context.md) — what zetta already has that pub/sub builds on
   (Capy `task`, executors, `strand`, `async_event`), and the rules it must obey.
2. [`research/01-in-memory-fifo.md`](research/01-in-memory-fifo.md) — how others
   build an in-memory, thread-safe, coroutine-aware queue and fan-out: channels,
   lock-free MPMC, the Disruptor ring, Kafka/NATS logs, P0260.
3. [`research/02-growable-rings-and-delivery.md`](research/02-growable-rings-and-delivery.md)
   — segmented rings and reclamation, inline vs heap events, and how brokers
   choose recipients, ordering, priority and QoS.
4. [`decisions.md`](decisions.md) — settled decisions with cost and revisit
   condition (D1 locks + condition variables, D2 pluggable stores / growing ring,
   D3 runtime delivery semantics, D4 binary events with a heap, D5 no coroutines
   in stage 1, a mutex per queue).
5. [`stage-1-in-memory-fifo.md`](stage-1-in-memory-fifo.md) — the first slice: an
   in-memory topic with a list of subscribers. Draft, revision 2.
6. [`questions.md`](questions.md) — open questions, answered ones kept with their
   answers.

## Stages (proposed, not agreed)

| Stage | What | Status |
|---|---|---|
| 1 | In-memory topic on one node, thread-safe, thread subscribers | designing |
| 2 | The same API across the cluster (a topic owned by a partition, subscribers on any node) | not started |
| 3 | Retention / replay (a log with per-subscriber offsets) | not started |
| 4 | Change events from tables and datasets (CDC) as topics | not started |
