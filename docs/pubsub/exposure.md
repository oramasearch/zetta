# Exposure on MCP and REST

**Issues:** [#1033](https://github.com/oramasearch/zetta/issues/1033),
[#1076](https://github.com/oramasearch/zetta/issues/1076) · **Decision:** D22 ·
**Code:** `src/core/api/pubsub_capabilities.cpp` · **Tests:**
`tests/pubsub/test_pubsub_capabilities.cpp`, `tests/api/test_rest_server.cpp`,
`tests/test_runtime.cpp`

Pub/sub is engine-internal by default. Ten capabilities are registered on every node
(each renders as a REST route, an MCP tool and an OpenAPI entry), but only topics whose
`exposed` setting is on are reachable through them; an unexposed topic answers
`not_found`, as if it did not exist.

| Capability | Kind | Right | What |
|---|---|---|---|
| `create_topic` | action | `topics:write` | create with full settings; `exposed` defaults to false |
| `drop_topic` | action | `topics:write` | drop an exposed topic |
| `list_topics` | read | `topics:read` | exposed, authorized topics with subscribers, held events, next sequence |
| `get_topic` | read | `topics:read` | full settings and per-subscription positions/lag |
| `update_topic` | action | `topics:write` | mutable settings, immediate retention shrink |
| `browse_topic` | read | `topics:read` | non-consuming forward/backward browse and async long poll |
| `publish_event` | action | `events:write` | publish to an exposed topic; `from` is the caller; clamped fields reported |
| `update_event` | action | `events:write` | revision-checked payload edit, opt-in redelivery |
| `delete_events` | action | `events:write` | remove held sequences or an exclusive range |
| `receive_events` | read | `pubsub:subscribe` | events from a sequence number, oldest first; `wait_ms` long-polls (up to 30 s) |

`receive_events` is stateless: pass the returned `next_sequence` to continue. For REST,
the handler registers a one-shot `runtime::async_event` with the topic before its
initial no-wait read. If that read is empty, the route suspends on the event, bounded by
`wait_ms` through `runtime::timeout`; publish or topic close signals every registered
waiter. The route then reads once more. Both reads use the normal authorization + audit
path on the owner loop, so a long poll makes at most two governed calls. MCP
`tools/call` uses the same async event and bounded wait for awaitable reads.
The same mechanism serves `browse_topic`; it never creates or advances a
subscription. See [crud.md](crud.md) for schemas, examples, revisions and D39.

The runtime event adapter serializes registration, publish and cancellation cleanup
across threads. Its lock is held only while updating the event's waiter list; suspended
HTTP requests hold neither that lock nor a thread.

**Capacity:** a waiting call holds a socket and a session coroutine, but no thread.
The accept loop never waits for a free session: it accepts first and allocates a
session on demand. The old fixed pool waited for an idle worker **before** `accept()`;
once every worker held a poll, new connections filled the kernel backlog and then
timed out during TCP connect. Merely increasing that pool did not remove the coupling.
The accept loop yields to ready session work after every 32 accepts, so a continuous
reconnect burst cannot monopolize the IO executor. Startup logs the configured cap,
idle cache and requested backlog; saturation logs the 503 policy.

The regression holds 500 polls with `from_sequence` past the topic head and
`wait_ms=10000`, then opens 20 fresh no-wait connections. Each must connect and answer
within 5 ms, even with the idle parser cache limited to one object.

Remaining per-node limits are explicit in `ServerOptions`:

| Option | Default | Meaning |
|---|---:|---|
| `max_connections` | 4096 | Admitted HTTP sessions, including long polls, keep-alive and clients still sending headers/bodies. At capacity the listener continues accepting and returns HTTP 503 with `error.code=busy`, `Retry-After: 1` and `Connection: close`. |
| `idle_connection_cache` | 32 | Completed parser/serializer objects retained for reuse. Cached objects hold no open socket; this is not a limit on active sessions. |
| `listen_backlog` | 1024 | Requested kernel queue for pending TCP connections; the OS can clamp it. Held, admitted polls do not occupy this queue. |

The application cap is not a promise of unlimited capacity: process/system file
descriptor limits, memory and kernel queues still apply. Provision descriptors for
the chosen session cap plus the listener, a transient overload-rejection socket, and
the node's other files and connections. If `accept()` reports resource exhaustion,
the server logs the error and retries after an asynchronous delay so existing sessions
can finish. The overload response write is bounded to 50 ms; it never waits for a
held poll to finish. Shutdown wakes polls and drains their owner-thread work before
releasing sessions. No new capability schema or JSON format is introduced.

**Streaming subscriptions over SSE are a later slice**; the long poll is what stage 1
ships. No new JSON format beyond the capability schemas (in
`schemas/openapi.json`).
