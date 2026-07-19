# Create a dataset from a connector query

A **source connector** runs a read-only query against an external system and
returns rows; [`add_dataset_rows`](add-dataset.md#add_dataset_rows--the-in-memory-sibling-the-connector-query-landing-path)
then shards and places those rows across the cluster. Together they close the gap
between *"Connect the sources"* and an actual clustered, featurizable dataset
([#827](https://github.com/oramasearch/zetta/issues/827)).

Until this landed, zetta could only ingest a **local file**, so data living in a
warehouse had to be exported by hand first. `StepKind::Connect` records a lineage
string but materializes nothing, and the only `DatasetResolver` reads local paths.

```
include/zetta/connect/connector.hpp          ConnectorConfig / ConnectorRows / run_connector_query
include/zetta/connect/distributed_fetch.hpp  the result-set tiling + the concurrent partition read
src/core/connect/distributed_fetch.cpp       plan_partition_assignments / submit / fetch_result_partitions
src/core/connect/snowflake_protocol.hpp/.cpp the SQL API v2 wire details, shared by both fetch paths (internal)
include/zetta/connect/connection_store.hpp   SavedConnection — the replicate-to-all credential store
src/core/connect/connection_store.cpp        save/find/list/remove over the reserved zetta_connect.connections table
include/zetta/connect/http_transport.hpp     HttpRequest / HttpResponse / HttpTransport + AsyncHttpTransport seam
src/core/connect/connector.cpp               the config boundary, the read-only guard, the Snowflake connector
src/core/connect/http_transport.cpp          the installed-default transport registry (no HTTP library in zetta-core)
include/zetta/api/native_http_transport.hpp  install/shutdown the native client
src/core/api/native_http_transport.cpp       the native coroutine HTTPS client (zetta-api target)
tests/connect/test_connector.cpp             the boundary, the guard, and Snowflake against a fake transport
tests/connect/test_connection_store.cpp      the store, its boundary, and the no-leak contract
tests/connect/test_distributed_fetch.cpp     the tiling's exactly-once property + the bounded concurrent read
tests/connect/test_native_http_transport.cpp the native client against a real loopback server
```

## The shape

**The connector's only job is to produce rows; Zetta owns the distribution.**

```
run_connector_query(config, sql, max_rows, transport)   ← rows out of the source
        ↓
data::add_dataset_rows(node, rows, options)             ← shard + place across the cluster
        ↓
system.datasets / system.dataset_partitions             ← a partitioned, featurizable dataset
```

Because the landing path is the same `write_partitioned` +
[`RendezvousPlacement`](partitioned-write-path.md) used by every other dataset, a
connector-loaded table is indistinguishable downstream from a file-loaded one: it
featurizes [compute-near-data](../compute/compute-near-data.md) exactly the same way.

## The capability: `create_dataset_from_query`

One registry entry renders as an MCP tool, `POST /v1/create_dataset_from_query`,
and an OpenAPI operation (`src/core/api/capabilities.cpp`):

```jsonc
{
  "connector": {                       // the ConnectorConfig, embedded
    "type": "snowflake",
    "account": "ab12345.us-east-1",
    "token": "<PAT>",                  // input-only, never echoed back
    "warehouse": "COMPUTE_WH",
    "database": "ANALYTICS", "schema": "PUBLIC"
  },
  "query": "SELECT account_id, mrr, churned FROM churn",
  "database": "churn", "table_name": "accounts",   // where it lands in Zetta
  "max_rows": 100000,                              // cap pulled from the source
  "partitioned": true, "partition_count": 8        // how it spreads
}
```

It returns the landed dataset — `{database, table, rows, columns, partitions[]}`
— with each partition's `owner_nodes`, so the caller sees the placement across
the cluster immediately.

### A result set is a cache: RF=1

`create_dataset_from_query` passes `replication_factor = 1` explicitly, against
the engine's durable default of **3** ([add-dataset.md](add-dataset.md)). That is
not an oversight — a connector result set exists for processing, is always
re-pullable from its source, and is cheap to lose. Paying for three copies of a
cache is paying three times for something the source already holds. If a
partition's owner is lost, the remedy is re-running the ingest; a `SELECT` says
so rather than answering short (see
[scatter-gather-select.md](scatter-gather-select.md)). A result set that turns
out to be worth keeping is upgraded deliberately with
[`promote_dataset`](promote-dataset.md), not by default.

**Consent-gated** (`requires_consent = true`): unlike `add_dataset`, this reaches
an external system over the network *with your credentials*, so it routes through
the consent gate rather than running silently. Governance-wise it is an
`Orchestration / create_dataset_from_query` operation carried by the
`pipelines:manage` permission.

## No foreign runtime (ADR-17)

There is **no warehouse driver or SDK in the engine**. A connector builds plain
HTTP requests against the source's public REST API and parses JSON. Even the HTTP
exchange is injected, through the `HttpTransport` seam:

```cpp
using HttpTransport      = std::function<HttpResponse(const HttpRequest&)>;
using AsyncHttpTransport = std::function<runtime::io_task<HttpResponse>(const HttpRequest&)>;
```

This is the same **injected-transport** pattern as the
[`CommandRunner`](../cloud/provisioners.md) behind the CLI cloud control planes: it
keeps connectors unit-testable against a fake (no network, no credentials, no
warehouse — every case in `tests/connect/test_connector.cpp` runs that way), and
keeps the real egress path in one replaceable place.

The real transport is a **native coroutine HTTPS client**, not a subprocess —
see [../api/native-http-transport.md](../api/native-http-transport.md). It lives
in the `zetta-api` target and installs itself into this seam at startup, so
zetta-core still links no HTTP library and no connector ever sees an HTTP type.
The async shape is what the distributed fetch fans out with
(`runtime::when_all` over N in-flight partition reads).

### Credentials never leak

`HttpRequest::secret_headers` names the headers whose values are secret (the
`Authorization` bearer). A transport must send them and then forget them: no
secret value may appear in a reason string, a log line, a span attribute, or a
process argument. The native client builds every failure reason from the host,
port, and the OS/TLS error only — never from a header — and there is no
subprocess, so nothing is visible in the process table.
`ConnectorConfig::to_json()` likewise **never emits the token**, so a config is
safe to echo back to a caller or log.

## Using the cluster's bandwidth, not one node's

A large result does not come back as one document. Snowflake's SQL API v2 returns
a statement handle plus `resultSetMetaData.partitionInfo[]` — **N independently
fetchable result partitions**, each read with
`GET /statements/{handle}?partition=i`. Reading them one after another makes the
whole ingest as slow as the coordinating node's single link to the source, which
is precisely the shape the repo's distributed-first rule forbids.

So the result set is treated as a **work set to be tiled**:

```
submit once            POST /statements  →  handle + partitionInfo[] (+ partition 0 inline)
        ↓
tile deterministically plan_partition_assignments(roster, rowCounts)
        ↓                 disjoint slices, balanced BY ROW COUNT
fetch concurrently     each node pulls its own slice, several requests in flight
        ↓                 runtime::when_all over a bounded wave, each under runtime::timeout
land where you pulled  the rows a node fetched become partitions that node owns
```

**Balanced by rows, not by index.** Result partitions are uneven, so a
round-robin over indices leaves one node holding the tail and the fetch takes as
long as that node's link. The tiling is longest-processing-time-first: the
biggest remaining partition goes to whichever node is currently carrying the
fewest rows.

**Deterministic, so no plan travels.** `plan_partition_assignments` is a pure
function of (roster, row counts). Every node computes the identical tiling from
the same inputs, which is the same agreement-by-construction the SPMD compute
jobs get from the collective participant roster.

**Bounded.** The fan-out runs in waves of `kDefaultPartitionsInFlight` (4, capped
at 32), so a thousand-partition result costs a fixed number of sockets. Each
exchange is a fresh TLS handshake with full chain verification — the win from
more concurrency flattens quickly while the cost does not.

### Degrading

| Situation | What happens |
| --- | --- |
| a single partition (small result) | it arrived inline with the submission; no second request is ever made |
| no async transport installed (a test, an egress-free build) | the partitions are walked **serially** over the synchronous transport — correct, just not concurrent |
| an explicitly injected transport (a fake) | the same serial walk, so a unit test can never reach the network through the installed async default |
| one partition fetch fails | the whole fetch fails cleanly with the partition index and the reason; nothing partial is landed |

`tests/connect/test_distributed_fetch.cpp` asserts the load-bearing property
directly: tile a result across 1–5 nodes, fetch every slice, and the union must
be the whole result set **exactly once** — no duplication, no loss — with each
partition read from the source exactly once across the whole cluster.

### What is not wired yet

The **cross-node** launch is not: today the tiling and the concurrent read are
used to parallelize the fetch *within* the coordinating node, and the
`plan_partition_assignments` tiling is the piece a multi-node co-launch will fan
out over (the `run.launch` signal, `docs/compute/coordinated-launch.md`, exactly
as the churn solution does). What blocks it is the **landing** half, not the
fetch: `add_dataset_rows` captures every row into the CRDT, so a peer that
fetched a slice would replicate those rows to the coordinator anyway — which
would put the rows on the wire a second time and negate the point. Landing a
peer's slice as partitions *it* owns without capturing the rows needs a
non-capturing partition-write path whose durability story (a table whose rows
live only in partition bodies) is a design decision in its own right. Saved
connections (below) exist so that when the launch lands, each node can present
the credential itself and the token never rides the launch envelope.

## Saved connections (the credential lives on every node)

Passing a token inline on every call is fine for a one-off, but it puts the
secret on the wire each time and — more importantly — it gives the *coordinating*
node the only copy. The distributed fetch needs **every** node to hold the
credential, because every node pulls its own stripe of the result straight from
the source. So a connection can be **saved by name**:

```jsonc
// create_connection
{"name": "warehouse", "connector": {"type": "snowflake", "account": "…", "token": "<PAT>", "warehouse": "COMPUTE_WH"}}

// then, per query — no credential in the request at all
{"connection": "warehouse", "query": "SELECT … FROM churn", "database": "churn"}
```

`create_dataset_from_query` takes **either** `connection` **or** `connector`,
never both; the handler says which when a call gives neither or gives both.

### Where it lives, and why that is a plain table

The store is an ordinary `data::Table` in a reserved database
(`zetta_connect.connections`). A table's `ReplicationStrategy` defaults to
**ReplicateToAll**, so the row reaches every member through the same CRDT gossip
as any other row — which is exactly the property a credential every node must
present needs. It is deliberately **not** partitioned.

| Capability | Kind | What it does |
| --- | --- | --- |
| `create_connection` | action | save (or replace) a named connection; returns it **redacted** |
| `list_connections` | read | every saved connection, credentials **absent** |
| `delete_connection` | action | remove one from every node |

All three are `Orchestration` operations under the `pipelines:manage` permission,
alongside `create_dataset_from_query`. `system.connections` renders the same
source as a read view.

### The token is in exactly one place

The credential is written into the store row — that *is* the feature. Everything
derived from it redacts:

| Surface | Carries the token? |
| --- | --- |
| the store row (`ConnectorConfig::to_storage_json()`) | **yes** — the one serialization that does, and the only reason it exists |
| `ConnectorConfig::to_json()` | no — cannot emit it |
| `SavedConnection::to_redacted_json()` | no — built from `to_json()`, so the redaction is structural, not a field someone must remember to strip |
| `create_connection` / `list_connections` output | no |
| `system.connections` | no |
| audit event details, log lines, error reasons | no |

`tests/connect/test_connection_store.cpp` asserts this directly rather than
trusting it: it searches every read surface — including a real
`SELECT * FROM system.connections` — for the secret and requires a miss, and
requires a hit in `to_storage_json()` alone.

Names are validated at the boundary: 1–64 characters of `[A-Za-z0-9_-]` starting
with a letter or digit. Saving an existing name **replaces** it in place, so
rotating a credential does not leave two rows racing to answer one lookup.

## In the dashboard

The web dashboard renders every action dialog straight from the capability
registry, so a new capability gets a form for free — but the renderer only ever
handled **flat scalar** properties. `create_dataset_from_query`'s `connector` is
a nested object, which meant it had no usable dialog at all: one opaque text box
where a structured input belongs.

`Data → Datasets → Load from a source` now renders it properly:

- a nested object property becomes its **own labelled group** of `parent.child`
  controls inside the dialog's grid, so the shape of the input is visible;
- an enum sub-property (the connector `type`) becomes a **picker**, not free
  text;
- the credential is a **masked** input (`token` / `secret` / `password` /
  `api_key` render as `type="password"`);
- a **saved-connection picker** sits above it — pick a name and leave the
  connector group blank, and the credential never enters the browser at all.

Decoding is the mirror image (`web_form_input`): scalar fields are coerced by
the schema exactly as the REST query-string path does, and an object property is
reassembled from its `parent.child` fields and coerced against its sub-schema.
**Empty fields are dropped**, which is what makes the either/or work from one
form — leave the connector group blank and only `connection` is sent; fill it in
and leave the picker on "none" and only `connector` is sent.

`Data → Source connections` is the management surface (save / delete), rendering
`system.connections` — names and settings, never a token.

## Read-only by construction

`run_connector_query` accepts only a **single `SELECT` (or `WITH`)** statement.
Anything that could mutate the source — `INSERT` / `UPDATE` / `DELETE` / `DROP` /
`COPY` / `CALL` — and any smuggled second statement (`SELECT 1; DROP TABLE …`) is
rejected at the boundary **before a request is built**, so a write never reaches
the network. Leading whitespace and `--` / `/* … */` comments are skipped so the
guard sees the first real keyword.

## Snowflake (the first connector)

Snowflake is reached over its **SQL API v2**: `POST /api/v2/statements` with a
bearer credential.

| Concern | How |
| --- | --- |
| Auth | `Authorization: Bearer <token>` + `X-Snowflake-Authorization-Token-Type: PROGRAMMATIC_ACCESS_TOKEN` (a PAT) or `OAUTH` |
| Session | `database` / `schema` / `warehouse` / `role` ride in the request body (a warehouse is required for token auth) |
| Async | a `202` means the statement is still running — the connector polls its `statementHandle` until it completes, then reads the result |
| Large results | the first partition arrives inline; the rest are fetched by index off the same handle (`?partition=N`), bounded by `max_rows` |
| Typing | the SQL API returns **every** value as a JSON string, so cells are coerced by their declared `rowType.type` — `fixed`/`real` → number, `boolean` → bool, empty → null — otherwise the landed dataset would be all strings and nothing downstream could featurize it |

Key-pair/JWT auth is not supported; a PAT or OAuth token is the credential.

## Boundary

`ConnectorConfig` is a value-type DTO with a hand-written, non-throwing
`from_json` mirroring `connector_config_schema()` (draft-07). It rejects an
unknown connector type, a missing/empty `account` or `token`, an `auth` other than
`pat`/`oauth`, a non-string field, and any unknown key. Nothing throws across the
seam: an unknown type, a rejected statement, a transport failure, an error status
from the source, and a malformed response all come back as a
`ConnectorResult::error`.

## Scope notes / later slices

- **Snowflake is the only connector** in this slice; a second one is a new branch
  in `run_connector_query` plus an entry in `connector_types()`.
- The **cross-node co-launch** of the fetch is a later slice — see *What is not
  wired yet* above for the landing-path decision it waits on.
- The native client does **no connection pooling**: one connection per exchange
  with `Connection: close`, so a fan-out pays one TLS handshake per request. A
  pooled variant is a change behind the same seam.
- **TLS is not covered by a test** — verifying it needs a certificate authority
  and a real peer; the loopback suite exercises the same coroutine over a plain
  socket, and the mTLS peer transport covers the Corosio TLS surface separately.
- A statement the source is still executing (a `202`) is polled on a
  **`runtime::delay` timer**, not a `sleep_for` ([#832](https://github.com/oramasearch/zetta/issues/832)):
  `submit_connector_statement_async` is a coroutine that suspends between reads,
  so a waiting poll occupies a timer slot instead of a thread. The synchronous
  `submit_connector_statement` is the one explicit `runtime::block_on` bridge for
  callers that are not coroutines.

## Adding another connector

Snowflake is the reference implementation, not a special case — a second
connector is a new entry in `connector_types()`, a `<vendor>_protocol.hpp/.cpp`
holding that source's wire details, and a dispatch branch in
`run_connector_query`. Everything else (the read-only guard, the credential
contract, the landing path, the capability, the dialog) is shared and needs no
change.

Follow the **new-connector** skill (`.claude/skills/new-connector/SKILL.md`) — it
carries the recipe plus the five non-negotiables every connector must satisfy.

See also [add-dataset.md](add-dataset.md) (the landing path),
[partitioned-write-path.md](partitioned-write-path.md) (how rows spread across the
cluster), and [ddf-featurize.md](../ddf/ddf-featurize.md) (what you do with the
dataset once it lands).
