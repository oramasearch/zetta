# Research: growable rings, inline vs heap events, runtime delivery semantics

> **Provenance.** Two Perplexity (`perplexity_ask`, high context) runs,
> 2026-09-27, after decisions D2–D4. **Status:** background; describes no zetta
> code.

## A deque of rings (segmented storage)

| System | Segment | Reclaim |
|---|---|---|
| moodycamel ConcurrentQueue | fixed blocks (default 32 entries) | spent blocks go to a free list and are reused [3][9] |
| Folly `UnboundedQueue` | fixed segments, each used once | freed only when no thread can still reference it; a lagging thread holds back the chain [2] |
| Aeron | 3 rotating power-of-two term buffers (64 KiB–1 GiB); framed variable-length records; `tryClaim` reserves then commits [4][7][10] | rotation |
| Chronicle Queue | time-based cycles (files) | roll-over [5] |

What carries over to us:

- **Every message gets a sequence number; each subscriber holds its own
  next-to-read sequence.** Crossing a segment boundary is following the
  segment's `next` link. A segment is reclaimable when **every subscriber
  entitled to read it** has passed it: reclaim in order up to the minimum cursor.
- **A slow subscriber pins segments.** Growth needs a limit or a visible signal,
  not silent unbounded memory.
- **Recycle segments through a bounded free list** (no allocation in steady
  state), free the excess.
- Segment size: a few hundred to a few thousand slots is the usual starting
  point; make it configurable and measure.

## Inline vs out-of-line events

Two shapes:

1. **Fixed-size slots**: each slot holds a header and a small inline area; a
   larger payload goes out-of-line and the slot holds a length and pointer. Slots
   stay uniform; every slot pays for the inline area.
2. **Framed records packed into a byte segment** (Aeron): header + payload
   contiguous, variable length, padding at segment end. Compact; out-of-line only
   for events bigger than a segment.

For broadcast, an out-of-line payload read by many subscribers should be
**immutable and refcounted** (released when the last entitled subscriber passes
it), and ideally come from a size-class pool rather than a raw `new` per event.

## Runtime delivery semantics: what others fix, and where

| Axis | Others | Where it is chosen |
|---|---|---|
| Recipients | RabbitMQ exchange types; NATS plain subscribers (all) vs queue groups (one of the group) [4][9]; Kafka: one consumer per group [8] | at **subscription** time (which group you join) |
| Ordering | Kafka per partition/key [3]; Service Bus per session [1] | by topology, not per message |
| Priority | RabbitMQ priority queues: per-message priority, few levels recommended, the queue must opt in [5] | per **message**, within an opted-in queue |
| Guarantee | MQTT QoS 0/1/2 per publish, capped by the subscription's QoS [7] | per **message**, capped per subscription |

**Pitfalls of mixing them on one queue:** priority breaks FIFO; strict priority
starves low priority unless something ages it; strict order means head-of-line
blocking; redelivery reorders and duplicates (consumers must be idempotent for
"at least once"). "Deliver to exactly N" is not a feature anyone surveyed ships;
NATS queue groups come closest (one per group, N groups = N recipients).

## Sources

Run 1: [2] https://github.com/facebook/folly/blob/main/folly/concurrency/UnboundedQueue.h ·
[3] https://github.com/KjellKod/Moody-Camel-s-concurrentqueue/blob/master/README.md ·
[4] https://aeron.io/docs/aeron/log-buffers-images/ ·
[5] https://github.com/OpenHFT/Chronicle-Queue ·
[7] https://www.javadoc.io/static/io.aeron/aeron-client/1.41.5/io/aeron/Publication.html ·
[9] https://github.com/cameron314/concurrentqueue ·
[10] https://aeron.io/docs/aeron/publications-subscriptions/

Run 2: [1] https://learn.microsoft.com/en-us/azure/service-bus-messaging/message-sessions ·
[3] https://docs.redhat.com/en/documentation/red_hat_streams_for_apache_kafka/3.1/html/kafka_configuration_tuning/con-consumer-config-properties-str ·
[4] https://docs.nats.io/learn/core-nats/queue-groups ·
[5] https://www.rabbitmq.com/docs/priority ·
[7] https://public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html ·
[8] https://kafka.apache.org/25/javadoc/org/apache/kafka/clients/consumer/KafkaConsumer.html ·
[9] https://docs.nats.io/concepts/queue-groups
