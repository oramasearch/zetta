# Research: topic auto-creation, and durability across shutdown and crash

> **Provenance.** One Perplexity (`perplexity_ask`, high context) run,
> 2026-09-27, after a request for runtime-defined, persisted topics and
> Kafka-style auto-creation. **Status:** background; describes no zetta code.

## Auto-creation

| System | How | Notes |
|---|---|---|
| Kafka | `auto.create.topics.enable`: a metadata request for a missing topic creates it with broker defaults (`num.partitions`, `default.replication.factor`) [3][9] | often **disabled in production**: a typo creates a topic, and it silently inherits generic defaults; with ACLs the client needs `CREATE` on the cluster or topic |
| Pulsar | `allowAutoTopicCreation` + `allowAutoTopicCreationType` (partitioned or not), overridable per namespace [1][7] | the override per namespace is the useful part |
| NATS JetStream | none: a stream must be configured to capture a subject | |
| RabbitMQ | clients *declare* queues; declaring creates if missing, and fails if the existing queue has incompatible properties [2] | declaration, not implicit creation |

## Durability across graceful shutdown and crash

| System | Mechanism | Crash window |
|---|---|---|
| Kafka | writes through the page cache; `log.flush.interval.*` usually left to the OS; a recovery-point checkpoint; after an unclean shutdown, the tail past the checkpoint is scanned and repaired; replication is the real durability [3][9] | whatever the OS had not flushed, unless a replica has it |
| NATS JetStream | file streams `fsync` every `sync_interval` (default 2 min); `always` syncs before acknowledging [4][10] | up to 2 min by default |
| RabbitMQ | durable queue + persistent message; a publisher confirm means it is on disk [2][8] | none for confirmed messages |
| Redis AOF | `appendfsync always` / `everysec` / `no` | none / ~1 s / OS-dependent |

**The common design:** a topic is a sequence of append-only **segment files**, an
**index** from sequence number to file position, and a **checkpoint** of the
last known-good point. A graceful shutdown flushes and writes a clean checkpoint.
After a crash, recovery loads the checkpoint, scans the tail past it, truncates a
partial last record, and rebuilds the index for that tail.

## Sources

[1] https://pulsar.apache.org/docs/next/reference-pulsar-admin/ ·
[2] https://www.rabbitmq.com/docs/queues ·
[3] https://kafka.apache.org/41/configuration/broker-configs/ ·
[4] https://docs.nats.io/reference/config/jetstream/sync_interval ·
[7] https://apache.googlesource.com/pulsar/+/refs/tags/v3.2.3-candidate-1/conf/broker.conf ·
[8] https://www.rabbitmq.com/docs/confirms ·
[9] https://docs.redhat.com/en/documentation/red_hat_streams_for_apache_kafka/2.9/html/kafka_configuration_tuning/con-broker-config-properties-str ·
[10] https://github.com/nats-io/nats.docs/blob/master/nats-concepts/jetstream/README.md
