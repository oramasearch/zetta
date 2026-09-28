# Exposure on MCP and REST

**Issues:** [#1033](https://github.com/oramasearch/zetta/issues/1033),
[#1076](https://github.com/oramasearch/zetta/issues/1076) · **Decision:** D22 ·
**Code:** `src/core/api/pubsub_capabilities.cpp` · **Tests:**
`tests/pubsub/test_pubsub_capabilities.cpp`, `tests/api/test_rest_server.cpp`,
`tests/test_runtime.cpp`

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

`receive_events` is stateless: pass the returned `next_sequence` to continue. For REST,
the handler registers a one-shot `runtime::async_event` with the topic before its
initial no-wait read. If that read is empty, the route suspends on the event, bounded by
`wait_ms` through `runtime::timeout`; publish or topic close signals every registered
waiter. The route then reads once more. Both reads use the normal authorization + audit
path on the owner loop, so a long poll makes at most two governed calls. MCP
`tools/call` uses the same event-based bounded suspension and final governed
read; neither rendering holds a thread while waiting (#1076, #1137).
Non-object MCP request bodies retain the JSON-RPC invalid-request response.

The runtime event adapter serializes registration, publish and cancellation cleanup
across threads. Its lock is held only while updating the event's waiter list; suspended
HTTP requests hold neither that lock nor a thread.

**Capacity:** a waiting call holds an HTTP worker coroutine and its small coroutine
frame, but no thread. The server has 1,024 connection workers by default; the stress
case keeps 500 long polls registered while a no-wait receive remains below 5 ms.
**Streaming subscriptions over SSE are a later slice**; the long poll is what stage 1
ships. No new JSON format beyond the capability schemas (in
`schemas/openapi.json`).
