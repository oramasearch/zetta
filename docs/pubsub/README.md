# Pub/sub docs

zetta's in-memory pub/sub (epic [#1024](https://github.com/oramasearch/zetta/issues/1024)):
addressed events with an email-style envelope, an inbox for every addressable
entity, runtime topics, delivery options set at any level and bounded by
governance, limits detected from the environment, spilling to disk, and
best-effort restart survival. **Agents are the main user.** The design, with every
decision and its cost, is [../plans/pubsub/](../plans/pubsub/README.md); these pages
describe what is built.

**Boundaries of stage 1:** one node (the cluster is stage 2, designed later), no
partitions, no coroutines (subscribers are threads; every queue has its own mutex
and condition variables), no durability guarantee (restart survival is best effort
and logged), no rate limits. New JSON formats: the on-disk event frame header only
([events.md](events.md)).

- [events.md](events.md) — addresses (`kind:name`, prefix-glob wildcards), the
  envelope (from, sender stack, to/cc/bcc, reply-to, correlation id, topics-chain),
  payloads (inline vs shared heap), MIME interning, envelope checks, the on-disk frame (#1025).
