# Research: memory limits, overflow policies, spilling to disk

> **Provenance.** One Perplexity (`perplexity_ask`, high context) run,
> 2026-09-27, after Steve asked for configurable overflow including spilling to
> disk. **Status:** background; describes no zetta code.

## What brokers do when a queue or stream is full

| System | Limit | Policies | Default |
|---|---|---|---|
| RabbitMQ | memory high watermark (40% of RAM); `x-max-length`, `x-max-length-bytes` per queue | memory alarm blocks publishers; CQv1 pages to disk from 50% of the watermark; length overflow: `drop-head`, `reject-publish`, `reject-publish-dlx` | `drop-head` [4][8] |
| NATS JetStream | `max_bytes` per stream; memory or file storage | `DiscardOld`, `DiscardNew` | file storage, `DiscardOld` [1][5][9] |
| Redis Streams | `MAXLEN` / `XTRIM` (exact or `~` approximate) | trim oldest | none; explicit [2][6] |
| Pulsar | backlog quota per namespace (size or age) | `producer_request_hold`, `producer_exception`, `consumer_backlog_eviction` | `producer_request_hold` [3][7] |

Nobody treats disk as an automatic, unlimited extension of memory: disk is a
second tier with **its own** cap and its own overflow policy.

## Pitfalls of spilling segments to disk

- **Latency cliff:** a subscriber reading spilled segments is suddenly much
  slower, and its reads compete with the writes that spilled them.
- **Spill is not durability:** writing overflow to a file without `fsync` frees
  memory but promises nothing across a crash. Durability is a separate decision
  (stage 3).
- **Cursors across tiers:** a subscriber's position must stay valid whether its
  next event is in memory or on disk; a sequence number (not a pointer) makes
  that simple.
- **Disk is finite too:** the disk tier needs a cap and a policy for when it is
  full; test slow-disk and full-disk cases.

## Sources

[1] https://docs.nats.io/reference/config/jetstream/ ·
[2] https://redis.io/docs/latest/develop/data-types/streams/ ·
[3] https://pulsar.apache.org/docs/next/reference-pulsar-admin/ ·
[4] https://www.rabbitmq.com/docs/maxlength ·
[5] https://docs.nats.io/learn/deployment/sizing-and-resources ·
[6] https://redis.io/docs/latest/commands/xtrim/ ·
[7] https://pulsar.apache.org/docs/next/admin-api-namespaces/ ·
[8] https://www.rabbitmq.com/docs/3.13/memory ·
[9] https://github.com/nats-io/nats.docs/blob/master/nats-concepts/jetstream/streams.md
