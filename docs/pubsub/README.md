# Pub/sub docs

zetta's in-memory pub/sub (epic [#1024](https://github.com/oramasearch/zetta/issues/1024)):
addressed events with an email-style envelope, an inbox for every addressable
entity, runtime topics, delivery options set at any level and bounded by
governance, limits detected from the environment, spilling to disk, and
best-effort restart survival. **Agents are the main user.** The design, with every
decision and its cost, is [../plans/pubsub/](../plans/pubsub/README.md); these pages
describe what is built.

**Boundaries of stage 1:** one node (the cluster is stage 2, designed later), no
partitions, thread-based subscribers (every queue has its own mutex and condition
variables), no durability guarantee (restart survival is best effort and logged), no
rate limits. REST and MCP long polls are the exception: they suspend a coroutine on a topic
event without holding a thread (#1076, #1136, #1137). New JSON formats: the on-disk event frame
header only ([events.md](events.md)).

- [events.md](events.md) — addresses (`kind:name`, prefix-glob wildcards), the
  envelope (from, sender stack, to/cc/bcc, reply-to, correlation id, topics-chain),
  payloads (inline vs shared heap), MIME interning, envelope checks, the on-disk frame (#1025).
- [stores.md](stores.md) — `EventStore`; the `TopicRingStack` (a deque of ring buffers,
  stack-then-ring locking, free list, spill and read-back, flush and restore); the
  deque-backed work `QueueStore` (#1026, #1136).
- [crud.md](crud.md) — node-local topic/message CRUD, non-consuming browse, revisions,
  explicit redelivery, scopes, hash-only auditing, store durability and REST/MCP parity (#1136).
- [topics.md](topics.md) — the `PubSub` facade; topics as runtime objects; auto-creation;
  subscriptions (start position, groups, retention, gap notices); inboxes; the wildcard
  cache (#1027, #1028).
- [delivery.md](delivery.md) — delivery options layered message > subscription > topic;
  recipients and claims; priority levels with aging; at-least-once and exactly-once (#1028).
- [governance.md](governance.md) — the `Authorizer` injection point; identity-stamped
  envelopes; the sender-stack checks; clamp or reject; audit (#1029).
- [dispatch.md](dispatch.md) — the `Dispatcher`: a thread per key, a pool past a
  threshold, inline; per-key order; logged switches (#1026).
- [limits.md](limits.md) — detecting memory, CPU and disk from cgroups, the Downward API
  and the host; derived limits; overflow policies; spilling and its log lines (#1030).
- [restart.md](restart.md) — the shutdown flush within a budget; best-effort reload,
  including after a crash (#1031).
- [drains.md](drains.md) — inbox drains by address kind; replies; dead letters; error
  topics (#1032).
- [node.md](node.md) — the node owns pub/sub: start and stop, the governance-backed
  authorizer, the `table:` drain, `system.pubsub_topics`, spans (#1029, #1031, #1032, #1034).
- [exposure.md](exposure.md) — `create_topic`, `drop_topic`, `list_topics`,
  `publish_event`, `receive_events`; exposed topics, coroutine long polls, and
  trainer-owned `run-metrics.<run>` topics (#1033, #1076, #1137).
