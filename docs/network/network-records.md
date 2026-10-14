# Workspace network records

Refs [#1281](https://github.com/oramasearch/zetta/issues/1281), the
[secure-networking decisions](https://github.com/oramasearch/orama-operator/blob/dev/docs/infrastructure/secure-networking.md),
[zetta-tunnel developer-tunnel contract](https://github.com/oramasearch/zetta-tunnel/blob/issue-3-developer-tunnel/docs/tunnel.md),
and [egress contract](https://github.com/oramasearch/zetta-tunnel/blob/issue-4-egress-gateway/docs/egress.md).

Decision D1 keeps two kinds of state separate:

- Lake's Postgres control plane holds desired connections, egress rules,
  tunnel grants and peer connections for the operator to enforce.
- The workspace's Zetta holds the records people inspect: who changed a
  connection or grant, tunnel sessions, and network decisions/activity.

`zetta::network` is a typed layer over the replicated
[`zetta::data::RecordStore`](../data/record-store.md). It uses record
collections rather than pub/sub topics because these records must survive a
restart, be installed on every live peer before acknowledgement, catch up after
a peer rejoins, support indexed pages, and expire by policy. Pub/sub topics are
node-local messages and do not provide that contract.

## Collections and retention

Node startup declares these definitions idempotently. Retention starts at the
record's last write; an expired record becomes unreadable immediately and each
node removes its local copy during its next sweep. The fixed definitions share
a stable creation value so independently started peers publish byte-identical
catalog entries and do not create definition conflicts when they meet.

| Record type | Collection | Retention | Main indexes |
|---|---|---:|---|
| `connection` | `network_connections` | 365 days | `event`, `workspace`, `connection_id`, `actor_id`, `source_id`, `target_id` |
| `grant` | `network_grants` | 365 days | `event`, `workspace`, `grant_id`, `user_id`, `actor_id` |
| `tunnel_session` | `network_tunnel_sessions` | 90 days | `event`, `workspace`, `session_id`, `grant_id`, `role` |
| `activity` | `network_activity` | 30 days | `event`, `workspace`, `role`, `decision`, `rule_id` |

Every collection also has the internal `all=all` index, ordered by the record's
UTC RFC3339 `timestamp`, so Lake's default read is newest-first. Connection and
grant lifecycle history is retained for a year; session history for a quarter;
the higher-volume decision stream follows the node's 30-day detailed-audit
default.

## Producer JSON

`network_record_schema()` is the source of truth and is published as
`schemas/network_record.schema.json` plus the `network_record` spec-index
entry. Every object has `schema: 1`, a UTC RFC3339 `timestamp`, and a non-empty
`workspace` routing label. Unknown fields and malformed identifiers are
refused before storage.

Lake writes complete lifecycle snapshots after its desired-state transaction:

- `connection_declared`, `connection_updated`, `connection_removed`: Lake's
  connection UUID, actor, source/target kinds and UUIDs, ports, and optional
  credential scope.
- `grant_created`, `grant_revoked`, `grant_expired`: Lake's grant UUID, actor,
  user, target, port, expiry and optional revocation time.

zetta-tunnel writes its existing producer objects without translating field
names:

- relay `session_start`, `session_end`, and `refusal` records carry the
  32-hex session id, grant UUID, peer/endpoint, reason and byte totals;
- `egress_decision` carries allow/deny, rule UUID, source, DNS name, port,
  protocol and resolved address;
- `peer_decision`, `ingress_decision`, and `mesh_decision` share the common
  source/destination/protocol/byte-count activity shape.

Endpoint-side tunnel records do not contain a workspace or grant. They remain
in the endpoint's local diagnostic sink and are not accepted by this workspace
API; the relay's record is authoritative for Lake.

Tunnel session start and end use the same `relay:<session_id>` key. The start is
therefore the live row; the end replaces it, moves it from the
`event=session_start` index to `event=session_end`, and preserves its final byte
totals. Other record keys are the SHA-256 of their canonical JSON, making an
identical retry idempotent while preserving distinct lifecycle events.

## zetta-tunnel write interface

`POST /v1/write_network_record` and MCP `write_network_record` accept the
record object itself, not a base64 wrapper. The capability schema is the
producer schema above. A successful response is:

```json
{
  "type": "tunnel_session",
  "key": "relay:0123456789abcdef0123456789abcdef",
  "revision": 123,
  "updated_ms": 1792010000000,
  "expires_ms": 1799786000000,
  "owner": "node-id",
  "replicas": 2,
  "replicas_expected": 2
}
```

The call requires `network:write` (`data:network_records_write`). It waits for
the record owner to fsync and for every reachable live peer to install the
write. `replicas < replicas_expected` reports a peer that will catch up through
anti-entropy.

## Lake read interface

`GET /v1/list_network_records`, its JSON-body POST alias, and MCP
`list_network_records` accept:

```json
{
  "type": "activity",
  "filter": "decision",
  "value": "deny",
  "direction": "desc",
  "limit": 100,
  "after": "optional-page-cursor"
}
```

`type` is `connection`, `grant`, `tunnel_session`, or `activity`. `filter`
defaults to `all`; otherwise it must name one of the collection's indexes and
requires `value`. The response returns decoded `record` objects with their key,
revision, update/expiry milliseconds, and an optional `next_after` cursor. The
call requires `network:read` (`data:network_records_read`). A Lake service role
needs `mcp:call, network:read`; zetta-tunnel needs `mcp:call, network:write`.
Neither permission grants the generic record API.

## Workspace isolation

Zetta does not multiplex tenants. The operator gives each workspace's Lake and
zetta-tunnel components the API address of that workspace's Zetta. The
`workspace` field is provenance and a routing check for the producer; it is not
a query tenant key. Records written to workspace A's Zetta do not exist in
workspace B's record log or replicated catalogue, and the acceptance test
starts two independent workspace nodes to prove that separation.

Per-connection credentials for apps are #1280. Zetta-to-Zetta peer links are
#1282. Neither is implemented here.

## Boundaries and tests

New JSON format: `zetta.NetworkRecord`, version 1. The capability input reuses
that boundary directly; stored bytes are an implementation detail of the
existing record log.

- `tests/network/test_network_records.cpp` covers fixed definitions and
  retention, schema rejection, session start/end replacement, indexes and an
  egress decision.
- `tests/api/test_network_capabilities.cpp` covers zetta-tunnel write → Lake
  read, distinct read/write roles, unauthorized callers, and two independent
  workspace Zettas.
