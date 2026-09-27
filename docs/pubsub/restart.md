# Shutdown and restart

**Issue:** [#1031](https://github.com/oramasearch/zetta/issues/1031) · **Decisions:** D17,
D18, D20 · **Code:** `src/core/pubsub/pubsub.cpp` (`start`, `shutdown`) · **Tests:**
`tests/pubsub/test_pubsub.cpp`

**Best effort, and logged** (D18). There is no durability guarantee in stage 1.

- `shutdown(budget)` stops accepting publishes, stops the maintenance thread and the
  dispatcher, closes every subscription, then writes each **default** topic's held
  events (spilled rings read back) to `level-N.frames` next to its definition, plus the
  next sequence number. The budget defaults to 25 s, under Kubernetes' 30 s default
  grace period (D20). A topic that does not fit in the budget is logged as lost.
  **Ephemeral** topics are never written.
- `start()` reloads every definition, then the flushed frames, then any spill files a
  crash left behind (merged by event id and sorted by sequence), and deletes them once
  loaded. A torn or corrupt tail is detected by the frame's length and crc32 and
  dropped, never parsed. Sequence numbers continue where they stopped; subscriptions
  do not survive (subscribers re-join; inboxes keep their events).

```
pubsub: INFO shutdown: flushing 14 topics, 3.1 GiB held; budget 25s
pubsub: INFO shutdown: flushed 14/14 topics in 11.4s
pubsub: INFO topic:orders recovered 1204339 events (next sequence 89325343)
pubsub: INFO start-up: reloaded 14 topics, 1204339 events; 0 torn or corrupt tails dropped
```

Zetta's SIGTERM / SIGINT handler (`src/core/signals.cpp`) sets the stop flag; the node's
shutdown path calls `PubSub::shutdown`.
