# Delivery: recipients, ordering, priority, guarantees

**Issue:** [#1028](https://github.com/oramasearch/zetta/issues/1028) · **Decisions:** D3,
D6, D9 · **Code:** `include/zetta/pubsub/delivery.hpp`, `src/core/pubsub/delivery.cpp`,
`src/core/pubsub/topic.cpp` · **Tests:** `tests/pubsub/test_delivery.cpp`,
`tests/pubsub/test_pubsub.cpp`

`DeliveryOptions` is one layer with every field optional; `resolve_delivery(message,
subscription, topic)` takes the most specific value per field, then the built-in default.

| Field | Values | Default |
|---|---|---|
| `recipients` | `All`, `One`, `Count(k)` | `All` |
| `ordering` | `Fifo`, `Priority` | `Fifo` |
| `priority` | 0–255 | 0 |
| `guarantee` | `AtMostOnce`, `AtLeastOnce`, `ExactlyOnce` | `AtMostOnce` |
| `ack_deadline` | redelivery deadline | 30 s |
| `reply` | `None`, `ReplyTo`, `ReplyAll` (drain results, D36) | the drain's default |

- **Recipients:** `All` is broadcast. A message asking for `One` or `Count(k)` carries a
  claim count; each subscription (outside a group) that reads it takes a claim, and the
  rest skip it. Groups give "one per group".
- **Priority:** a topic with `priority_levels > 1` keeps one store per level; a reader
  takes the highest non-empty level, except that the oldest event waiting longer than
  `priority_aging` goes first, so low priority is never starved.
- **At-least-once:** a delivery is in flight until `ack(sequence)`; after `ack_deadline`
  it is delivered again with `redelivery = true`.
- **Exactly-once:** at-least-once plus a per-subscription record of acknowledged event
  ids (the last 100,000), so a republish with the same `PublishRequest::id` is dropped.
  Exact within one process lifetime.
