# Pub/sub in a zetta node

**Issues:** [#1029](https://github.com/oramasearch/zetta/issues/1029),
[#1031](https://github.com/oramasearch/zetta/issues/1031),
[#1032](https://github.com/oramasearch/zetta/issues/1032),
[#1034](https://github.com/oramasearch/zetta/issues/1034) · **Code:**
`include/zetta/pubsub/node_integration.hpp`, `src/core/pubsub/node_integration.cpp`,
`src/core/node.cpp`, `src/core/query/executor.cpp` · **Tests:**
`tests/pubsub/test_node_pubsub.cpp`

`zetta::Node` owns one `PubSub` (`Node::pubsub()`, null while stopped or when
`NodeConfig::pubsub` is false). `start()` creates it on the data directory, logs the
detected limits, registers the `table:` drain and reloads what the last run flushed or
spilled. `stop()` flushes it within its budget before the replicated document is
persisted, so SIGTERM (headless loop exits on the stop flag → `stop()`) flushes pub/sub
(D20). `NodeConfig::pubsub_auto_create_prefixes` turns on auto-creation.

- **Governance** (`make_node_authorizer`): `user:<id or name>` principals are checked
  with `Node::authorize_user` for the `pubsub:*` permission (`pubsub:publish`,
  `pubsub:subscribe`, `pubsub:send_wildcard`, `pubsub:create`, `pubsub:manage`, in the
  permission vocabulary; admin holds them all), then with the object tier for topics
  (`ResourceKind::Topic`, prefix grants like blobs). `service:` principals are
  engine-internal and allowed; anything else is denied. Users and roles are owner-thread
  state, so the decision runs through `Node::run_on_owner`; while `stop()` waits for
  dispatched work it pumps the owner queue, so a drain can never deadlock a shutdown.
  **Not yet modelled by zetta's governance:** per-role delivery bounds (none apply) and
  delegation grants (every sender link is checked and currently allowed).
- **Tables** (`make_table_drain`): `table:<database>.<table>` inserts the event's JSON
  object payload as one row on the owner thread; anything else is an error on the
  table's error topic.
- **Observability**: `system.pubsub_topics` (topic, lifecycle, exposed, subscribers,
  held events, memory and disk bytes, spilling since, slowest subscriber and its lag,
  next sequence, dispatch mode), and a span in the node's ring for every publish,
  subscribe, ack, dead letter and topic create/drop (`pubsub:publish`, …) with its
  intent, plan and result shape.
