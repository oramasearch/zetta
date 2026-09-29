# Node-local topic and message CRUD

Issue [#1136](https://github.com/oramasearch/zetta/issues/1136). All operations
below are registry capabilities: the REST endpoint is `POST /v1/<name>` and
the MCP tool has the identical name and input/output schemas. Reads remain
classified as reads; `get_topic`, `list_topics`, and `browse_topic` also accept
GET query parameters. JSON examples below show request bodies and successful
`value` fields, inside the normal `ok/value/trace_id/span_id/duration_ms` envelope.

CRUD is **node-local**, exactly like publishes. Call the node holding the topic.
Sequence allocation and ownership are unchanged. Clustered behaviour follows
**D39**, which still defers clustered pub/sub: no follower revisions, replicated
payloads, cross-node sequence allocator, or deletion catch-up protocol is added.
Audit metadata uses the existing audit trail; message payloads never enter the CRDT.

## Permissions and concrete addresses

| Permission | Capabilities |
|---|---|
| `topics:read` | `get_topic`, `list_topics`, `browse_topic` |
| `topics:write` | `create_topic`, `update_topic`, `drop_topic` |
| `events:write` | `publish_event`, `update_event`, `delete_events` |
| `pubsub:subscribe` | `receive_events` and native subscriptions |

The existing per-tool `mcp:call` gate and topic-prefix grants also apply.
Prefix grants use literal prefixes, for example `allowed.`, without a wildcard.
Scoped-only operation grants are sufficient; adding a global operation grant
instead grants access to every topic, following the existing governance model.
Legacy `pubsub:create`/`pubsub:manage` and `pubsub:publish` grants remain aliases
for their corresponding write operations. A topic reader gains no subscription
permission. Listings filter out both unexposed and unauthorized concrete topics.

Only exposed topics are reachable; an internal topic returns `not_found`.
Wildcard sends (D30) still resolve normally, but CRUD requires each concrete
address. Inboxes (D28) use `inbox:<owner kind>.<owner name>`, for example
`inbox:agent.viewer`. An administrator may create an exposed inbox at that
address before its owner opens it. Automatically created inboxes remain
internal. Exposure is immutable, so this API cannot expose an existing internal
inbox or engine topic. An inbox uses the same owner and store as normal inbox delivery.

## Topic operations

`create_topic` accepts `topic` and a `settings` object containing every field
listed in [topics.md](topics.md#topicsettings). Legacy top-level `exposed`,
`lifecycle`, and `guarantee` are still accepted; supplying a setting twice is an
error. The response returns the normalized settings, including inherited fields
as null, as well as `topic` and `exposed`.

```json
{"topic":"orders","settings":{"exposed":true,"max_events":1000}}
```

The successful value has `topic: "topic:orders"`, `exposed: true`, and the full
`settings` object shown by `get_topic`, with `max_events: 1000`.

`get_topic` / MCP `get_topic`:

```json
{"topic":"orders"}
```

```json
{
  "topic":"topic:orders",
  "settings":{
    "lifecycle":"default","store":"ring_stack","ring_capacity":4096,
    "inline_threshold":256,"priority_levels":1,"priority_aging_ms":5000,
    "memory_cap_bytes":null,"disk_cap_bytes":null,"overflow":null,
    "over_bound":"clamp","producer_queue":false,"dispatch":null,
    "exposed":true,"topics_chain_max":32,"default_ttl_ms":null,
    "retention_ms":null,"max_events":1000,
    "delivery":{"recipients":null,"count":1,"ordering":null,"priority":null,
      "guarantee":null,"ack_deadline_ms":null,"reply":null},
    "bounds":{"max_priority":null,"max_recipients":null,"max_guarantee":null,
      "allowed_recipient_modes":[]}
  },
  "subscriber_count":1,
  "subscriptions":[{"id":1,"subscriber":"agent:worker","positions":[1],
    "next_sequence":1,"lag":1,"in_flight":0,"redeliveries":0,
    "connected":true,"group":""}],
  "held_events":1,"earliest_sequence":1,"next_sequence":2
}
```

`positions` are the store cursor per priority level; `next_sequence` is the
next held sequence that cursor can reach. `lag` counts held, unread events,
excluding deleted holes; group members share positions. In-flight and explicit
redelivery counts are separate. An empty topic's `earliest_sequence` equals its
`next_sequence`.

`list_topics` / MCP `list_topics` takes `{}` and returns:

```json
{"topics":[{"topic":"topic:orders","subscribers":1,"held_events":1,"next_sequence":2}]}
```

`update_topic` / MCP `update_topic` applies a settings merge patch. Omitted
fields are unchanged; null clears an optional override. Nested delivery and
bounds fields merge individually. Immutable fields are refused with a reason,
even if the supplied value equals the current value.

```json
{"topic":"orders","settings":{"max_events":0}}
```

The response is `{"topic":"topic:orders","trimmed":1,"settings":...}` with
the same full settings shape as above and `max_events: 0`. It removes the oldest
events immediately when a count or age retention limit shrinks. Retention also
runs on publish and maintenance. With explicit retention, ring-store consumption
does not release history; without it, existing subscriber-driven release rules
remain. Queue stores still remove each delivered event: their retention settings
bound undelivered work, and CRUD only addresses events still held by the store.
Retention age is measured from original publication, not editing. Default TTL
only affects future publishes and is distinct from held-event retention.

`drop_topic` / MCP `drop_topic` takes `{"topic":"orders"}` and returns
`{"topic":"topic:orders","exposed":true}`. It closes subscriptions and discards
the held events; their deletions are audited.

## Message operations

`publish_event` / MCP `publish_event` is unchanged:

```json
{"topic":"orders","payload":"original","encoding":"text"}
```

```json
{"event_id":"<event UUID>","accepted":true,"clamped":[]}
```

`browse_topic` / MCP `browse_topic` never creates a subscription, advances a
cursor, acknowledges, or claims an event. `from_sequence` is inclusive. Forward
defaults to oldest; backward defaults to newest. `limit` defaults to 100 and is
bounded to 1–1000. Gaps from retention or deletion are skipped.

```json
{"topic":"orders","from_sequence":1,"limit":100,"direction":"forward","wait_ms":0}
```

```json
{"topic":"topic:orders","events":[{"sequence":1,"id":"<event UUID>",
  "from":"user:<id>","published_at":1800000000000,"payload":"original",
  "size":8,"encoding":"text","headers":[],"revision":1,
  "edited_at":null,"edited_by":""}],"earliest_sequence":1,"next_sequence":2}
```

Timestamps are Unix milliseconds. `size` is payload bytes. Headers are an array
of sealed `{name,value}` objects. `next_sequence` is the allocator's high-water
mark, **not a page cursor**: for forward pagination use the last returned
sequence plus one; for backward use it minus one (stop at sequence 1).

`wait_ms` is bounded to 0–30000. An empty first read registers a one-shot topic
event before checking the store and suspends a coroutine, on **both REST and
MCP**. Publish, edit, or topic closure wakes it; timeout also performs one final
authorized read. There are at most two reads, not a thread per waiter. A wakeup
may still produce an empty page when the requested range excludes the new event.
Direct registry calls do not wait without the transport's async event.

`update_event` / MCP `update_event` replaces only the payload:

```json
{"topic":"orders","sequence":1,"payload":"edited","expected_revision":1,"redeliver":false}
```

```json
{"topic":"topic:orders","sequence":1,"revision":2,"edited_by":"user:<id>","edited_at":1800000001000}
```

The sequence, event id, original author/publication time, headers, TTL and delivery
options do not change. New events start at revision 1. A stale expected revision
returns `conflict` (REST 409); a no-longer-held event returns `not_found`.
Payload growth beyond the memory cap is refused.

Pending subscribers receive the edited payload. Already received/acknowledged
events do **not** get a new delivery by default. Existing unacknowledged deliveries
retain their acknowledgement deadline but their future retry uses the new
payload. With `redeliver: true`, each retained subscription that actually received
the event gets one explicit queued redelivery, flagged `Delivery::redelivery`.
This includes acknowledged exactly-once deliveries: the explicit request
bypasses id deduplication. Group peers that never received it are not included.
Repeated edits before receipt coalesce to the latest revision; explicit
redelivery replaces any older in-flight retry rather than duplicating it.
Disconnected subscriptions still within their retention keep that queue.
Neither edits nor redeliveries allocate a sequence or recreate released history.

`delete_events` / MCP `delete_events` requires exactly one selector:

```json
{"topic":"orders","sequences":[1,3]}
```

```json
{"topic":"orders","before_sequence":4}
```

Both forms return `{"topic":"topic:orders","removed":2}` when two held
events matched. The range is exclusive. A sequence list has 1–1000 unique
positive entries; missing/already-deleted entries are ignored. Deletion cancels
in-flight retries and explicit redelivery and pending cursors skip the holes.
It cannot retract a payload a caller already received. An I/O failure stops the
batch, returns an error, and audits any earlier successful removals.

## Audit and durability

Administrative payload updates and deletions record actor, time, topic,
sequence, revision, and SHA-256 before/after hashes. Deletion has a null after
hash. There is one record per changed event, including retention trimming caused
by `update_topic` and removal by `drop_topic`. Topic-setting updates have a
separate settings-hash record. Payloads are not copied into the audit trail.
Routine consumption and scheduled retention follow the existing store lifecycle;
they are not administrative delete calls.

Successful `receive_events`, `browse_topic`, `get_topic`, and `list_topics`
reads use the registry's windowed audit policy. The first read records an
actor/capability/topic session; five-minute summaries record call count and
events delivered only for windows that returned events or other data. An empty
polling window adds no row beyond the session start. Denied calls and handler
failures remain one row per call.
See [audit trail policy](../governance/audit-trail.md#audit-policy).

Durability follows [stores.md](stores.md) and [restart.md](restart.md), exactly
as publishes do. Default topics flush held edits and deletions at shutdown;
edits to an already-spilled ring rewrite that store file by atomic replacement.
Sparse slot metadata keeps deletion holes stable through reload and re-spill.
Deletion persists the sequence high-water mark before removing held data.
Ephemeral topics never persist events. This is still best-effort restart survival,
not a new transactional or crash-durability guarantee.

## Verification

`PubSubCrud.*`, the pub/sub store and capability tests, and
`RestServerFixture.PubSubCrudRestMcpSchemaParityAndCalls` cover delivery,
revision checks, retention, store restart, exposure, scopes, audit hashes,
OpenAPI/tool schema parity, and a REST plus MCP call for every CRUD operation.
`RegistryFixture.RegistersTheCoreCapabilities` includes the five additions in
the registry count and checks each capability by name.
`RestServerFixture.BrowseLongPollWorksOnRestAndMcpWithoutConsuming` checks both
async transports. The registry is shared by bus and workspace binaries; local
image builds are deferred because this host does not permit Docker.
