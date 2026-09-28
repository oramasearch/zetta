# Pub/sub tests

Store tests cover sparse spill rewrite/read-back and non-consuming queue inspection.
Facade tests cover revisions, pending/acknowledged delivery, explicit redelivery,
purge, retention and restart. Capability tests cover schemas, concrete exposed
inboxes, separated permissions, hash-only mutation audits, polling summaries,
and per-call denied-read records. Dispatcher tests cover
one- and multi-worker ordering/fairness, exact topic-count sizing, resize
hysteresis, between-job rebalance and the blocking guard; a disabled benchmark
records one- and four-worker throughput. See
[dispatch](../../docs/pubsub/dispatch.md) and
[CRUD verification](../../docs/pubsub/crud.md#verification).
