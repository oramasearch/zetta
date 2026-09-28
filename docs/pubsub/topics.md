# Topics, subscriptions and inboxes

**Issues:** [#1027](https://github.com/oramasearch/zetta/issues/1027),
[#1028](https://github.com/oramasearch/zetta/issues/1028) · **Decisions:** D13, D14, D16,
D17, D19, D23, D28, D30, D31 · **Code:** `include/zetta/pubsub/pubsub.hpp`,
`src/core/pubsub/{pubsub,topic}.cpp` (`topic.hpp` is internal) · **Tests:**
`tests/pubsub/test_pubsub.cpp`

## PubSub, the facade

`zetta::pubsub::PubSub` owns every topic and inbox. `start()` reloads what a previous
run left and starts the maintenance thread; `shutdown()` flushes ([restart.md](restart.md)).

- **Topics are runtime objects** (D14): `create_topic(identity, topic:<name>, settings)`,
  `drop_topic`, `list_topics`, `topic_settings`, `topic_stats`. A topic's definition is
  written to `<data_dir>/pubsub/topics/<hex of the address>/definition.json` when it is
  created, so it survives any restart. (Replicating definitions through the CRDT so every
  node sees them is part of stage 2's cluster design, D39.)
- **Auto-creation** (D16): `PubSubConfig::auto_create_prefixes` lists name prefixes for
  which publishing or subscribing to a missing topic creates it with
  `auto_create_settings`; governed by `pubsub:create`; logged and audited.
- **No partitions** (D13): a topic is one ordered sequence per priority level. Sequence
  numbers are per topic; order is only promised within a level's store.

## TopicSettings

Every field follows the standing rule. `delivery` (the topic's delivery layer),
`lifecycle` (`Default` or `Ephemeral`), `store` (`RingStack` or `Queue`),
`ring_capacity`, `inline_threshold`, `priority_levels` and `priority_aging`, memory and
disk caps, `overflow`, `over_bound` (clamp or reject), `producer_queue` (appends go
through the dispatcher, D23), `dispatch` (pin to a thread of its own or inline),
`exposed` (D22), `topics_chain_max`, `default_ttl`, held-event `retention`,
`max_events`, and topic `bounds`. The complete CRUD contract and examples are
in [crud.md](crud.md). All are accepted under `create_topic.settings` and
returned by create/get/update. API durations use the `_ms` names below.

| Settings field | Default | Mutable |
|---|---|---|
| `lifecycle` | `default` (`ephemeral` never persists events) | no |
| `store` | `ring_stack` (`queue` is a shared work queue) | no |
| `ring_capacity` | 4096 slots | no |
| `inline_threshold` | 256 payload bytes | no |
| `priority_levels` | 1; queue stores normalize to 1 | no |
| `producer_queue` | false | no |
| `dispatch` | null (node policy); `auto`, `per_key`, `pool`, `inline` | no |
| `exposed` | false | no |
| `priority_aging_ms` | 5000 | yes |
| `memory_cap_bytes`, `disk_cap_bytes` | null (node limits) | yes |
| `overflow` | null (guarantee default); `unbounded`, `skip_ahead`, `disconnect`, `hold_publisher`, `reject_new`, `spill_to_disk` | yes |
| `over_bound` | `clamp`; alternatively `reject` | yes |
| `topics_chain_max` | 32 | yes |
| `default_ttl_ms` | null; expiration for future publishes | yes |
| `retention_ms` | null; maximum age of held history | yes |
| `max_events` | null; maximum number of held events | yes |
| `delivery` | inherited optionals | yes |
| `bounds` | unbounded optionals, empty allowed modes | yes |

`delivery` has nullable `recipients` (`all`, `one`, `count`), `ordering` (`fifo`,
`priority`), `priority` (0–255), `guarantee` (`at_most_once`, `at_least_once`,
`exactly_once`), `ack_deadline_ms`, `reply` (`none`, `reply_to`, `reply_all`),
and `count` (defaults to 1). `bounds` has nullable `max_priority`,
`max_recipients`, `max_guarantee`, and `allowed_recipient_modes` (empty means
any). Topic bounds further restrict the authorizer's bounds on publish.
Changing defaults does not rewrite stored message-level delivery options.

## Subscriptions

`subscribe(identity, subscriber, topic, options)` returns a `Subscription` handle; the
subscriber is a thread that calls `receive(timeout)` (no coroutines, D5).

- **Start** (D19): at the next event by default; `Oldest` or a sequence number instead.
- **Groups**: subscriptions with the same `group` share one position, so each event goes
  to one member (a work queue). A `Queue` store always behaves this way.
- **Retention** (D31): destroying the handle disconnects. An `agent:` subscriber's
  subscription is kept for 24 hours by default (`options.retention` overrides); anything
  else is removed at once. The maintenance pass removes expired ones.
- **Gaps**: when events a subscriber had not read were dropped (skip-ahead, lag limit),
  the next `Delivery` carries `missed`, and `Subscription::missed()` the running total.

## Inboxes (D28)

Every addressable entity has an inbox: `open_inbox(identity, owner)` subscribes the owner
to it from the oldest waiting event. Sending to `agent:405` before it connects creates
its inbox; other kinds need a drain ([drains.md](drains.md)) or an open inbox, and
otherwise dead-letter as "no such address". An inbox that nobody reads for its retention
(24 hours for agents) is closed and what is left in it is dead-lettered.

## Wildcards (D30)

A wildcard in `to`, `cc` or `bcc` resolves against the topics and inbox owners that
exist. The resolution is cached per pattern (bounded, `wildcard_cache_limit`) and the
cache is cleared whenever the set of topics, inboxes or subscriptions changes, so a
wildcard reaches whoever matches at send time.
