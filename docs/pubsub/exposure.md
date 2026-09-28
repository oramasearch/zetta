# Exposure on MCP and REST

**Issue:** [#1033](https://github.com/oramasearch/zetta/issues/1033) · **Decision:** D22 ·
**Code:** `src/core/api/pubsub_capabilities.cpp` · **Tests:**
`tests/pubsub/test_pubsub_capabilities.cpp`

Pub/sub is engine-internal by default. Five capabilities are registered on every node
(each renders as a REST route, an MCP tool and an OpenAPI entry), but only topics whose
`exposed` setting is on are reachable through them; an unexposed topic answers
`not_found`, as if it did not exist.

| Capability | Kind | Right | What |
|---|---|---|---|
| `create_topic` | action | `pubsub:create` | create a topic; `exposed` defaults to false; `lifecycle`, `guarantee` |
| `drop_topic` | action | `pubsub:manage` | drop an exposed topic |
| `list_topics` | read | `pubsub:subscribe` | exposed topics with subscribers, held events, next sequence |
| `publish_event` | action | `pubsub:publish` | publish to an exposed topic; `from` is the caller; clamped fields reported |
| `receive_events` | read | `pubsub:subscribe` | events from a sequence number, oldest first; `wait_ms` long-polls (up to 30 s) |

`receive_events` is stateless: pass the returned `next_sequence` to continue. It runs off
the owner loop (a long poll must not hold it), on the runtime's **blocking pool**
(`runtime::offload_blocking`, 64 threads), never on the compute pool, whose one thread
per core the waits would otherwise starve (#1063). Its authorization + audit gate still
runs on the owner thread. A surface that calls it on the owner thread (MCP `tools/call`)
gets an immediate answer, as with `wait_ms` 0.

**Capacity:** each waiting call holds one blocking-pool thread and one HTTP connection
worker (256), so a node serves up to **64 concurrent long polls** without delaying
other calls. Beyond that, calls queue for a free thread. An awaitable wait with no
thread per waiter is the follow-up. **Streaming subscriptions over SSE are a
later slice**; the long poll is what stage 1 ships. No new JSON format beyond the
capability schemas (in `schemas/openapi.json`).
