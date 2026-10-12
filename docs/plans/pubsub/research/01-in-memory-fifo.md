# Research: in-memory, thread-safe, coroutine-aware FIFO and fan-out

> **Provenance.** Two Perplexity (`perplexity_ask`, high context) runs,
> 2026-09-27. Prompts: (1) survey of coroutine channels vs lock-free MPMC vs
> Disruptor, and what C++26 / P0260 offer; (2) Disruptor multicast, Kafka/NATS
> log-with-offsets vs per-subscriber copies. **Status:** background; describes no
> zetta code.

## The distinction that matters most

"Thread-safe", "lock-free", "coroutine-aware" and "broadcast" are **four separate
properties**; none implies another. In particular:

- A **queue / channel** is a *work queue*: each message goes to **one** consumer
  (competing consumers). Boost.Cobalt channel, Asio `concurrent_channel`, libcoro
  `queue`, folly `UnboundedQueue`, moodycamel, Vyukov MPMC, and P0260 all work
  this way.
- **Pub/sub** is *broadcast*: every subscriber gets every message. You get it
  with either one queue per subscriber, or one shared buffer plus one cursor per
  subscriber.

## Designs

| Design | How a waiting consumer waits | Full buffer | Fan-out |
|---|---|---|---|
| Coroutine channel (Cobalt, Asio `concurrent_channel`, libcoro, folly::coro) | suspends; stored as a pending waiter; resumed by posting to its executor. Cobalt's channel is **not** thread-safe; its docs point to Asio's `concurrent_channel` for cross-thread use [3][6] | producer suspends; capacity 0 = rendezvous | competing consumers |
| Lock-free MPMC (Vyukov bounded, moodycamel) | doesn't wait at all; needs a wrapper (semaphore/cv, or an async waiter list) | Vyukov: enqueue fails; moodycamel: grows unless `try_enqueue` [8] | competing consumers |
| LMAX Disruptor ring | each consumer waits for a sequence; wait strategies are blocking (lock + cv, the default), yielding, busy-spin [3b][6b] | producer is gated by the **slowest** consumer's sequence | **multicast**: payload stored once, a cursor per consumer |
| Kafka partition / JetStream stream | consumer polls from its offset | retention by time/size; a slow consumer falls off the end | per consumer group; replayable [1b][8b] |
| NATS core | pushed to live subscribers | none: fire-and-forget, no replay [5b] | broadcast to current subscribers |

## Trade-offs for us

- **Per-subscriber queue:** simple, each subscriber gets its own capacity and
  its own slow-consumer policy; costs a copy (or a refcount bump) per subscriber
  per message, and publish work grows with subscriber count.
- **Shared ring + cursors (Disruptor):** payload stored once, no per-subscriber
  copy; the slowest subscriber limits the producer unless we choose to overwrite
  (then that subscriber must learn it missed messages).
- **Log + offsets (Kafka):** the ring grown into retention; gives replay. That is
  stage 3, but a ring with sequence numbers is its natural precursor, so the
  stage 1 choice decides how easy stage 3 is.

## Standards

- **`std::execution` (P2300, C++26)** is an execution model (senders/receivers),
  not a queue. Capy is our execution layer instead.
- **P0260 "C++ Concurrent Queues"** (R17, 2025) proposes `async_push`/`async_pop`
  returning senders. A work queue, not broadcast; not confirmed as adopted into
  C++26 [1][10]. Worth reading for its bounded/closed-queue semantics.

## Sources

Run 1: [1] https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p0260r12.html ·
[3] https://www.boost.org/doc/libs/latest/libs/cobalt/doc/html/index.html ·
[6] https://www.boost.org/doc/libs/1_84_0/libs/cobalt/doc/html/index.html ·
[8] https://github.com/cameron314/concurrentqueue ·
[10] https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p0260r17.html

Run 2: [1b] https://kafka.apache.org/24/getting-started/introduction/ ·
[3b] https://github.com/lmax-exchange/disruptor/blob/master/src/docs/asciidoc/en/user-guide/10_using_the_disruptor.adoc ·
[5b] https://docs.nats.io/concepts/jetstream ·
[6b] https://lmax-exchange.github.io/disruptor/javadoc/index-all.html ·
[8b] https://docs.nats.io/learn/jetstream/your-first-stream
