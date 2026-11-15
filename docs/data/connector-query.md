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
src/core/connect/distributed_fetch.cpp       plan_partition_assignments / plan_fetch_assignments / submit / fetch_result_partitions
include/zetta/connect/distributed_ingest.hpp the CROSS-NODE ingest: co-launch, fetch a tile, land it, agree
src/core/connect/distributed_ingest.cpp      run_distributed_connector_ingest + the connector_fetch launch runner
src/core/connect/snowflake_protocol.hpp/.cpp the SQL API v2 wire details, shared by both fetch paths (internal)
include/zetta/connect/connection_store.hpp   SavedConnection — the replicate-to-all credential store
src/core/connect/connection_store.cpp        save/find/list/remove over the reserved zetta_connect.connections table
include/zetta/connect/http_transport.hpp     HttpRequest / HttpResponse / HttpTransport + AsyncHttpTransport (the injected transport)
src/core/connect/connector.cpp               the config boundary, the read-only guard, the Snowflake connector
src/core/connect/http_transport.cpp          the installed-default transport registry (no HTTP library in zetta-core)
include/zetta/connect/mysql_client.hpp       MySQL: the native protocol as a sans-I/O session + the runner injection point (#895)
src/core/connect/mysql_client.cpp            handshake, TLS negotiation, auth plugins, read-only fencing, typed text result set
include/zetta/api/mysql_protocol.hpp         install_mysql_protocol — installs the MySQL TCP/TLS driver
src/core/api/mysql_protocol.cpp              the coroutine TCP + TLS-upgrade driver (zetta-api target)
src/core/api/system_trust_store.hpp/.cpp     the system CA roots, shared by the HTTPS client and the MySQL driver
src/core/connect/lake_sql_protocol.hpp/.cpp  lake's SQLite/DuckDB/Polars services over HTTP: endpoint, guard, request, typed answer, Polars dataset list (internal, #1014)
include/zetta/connect/source_catalog.hpp     list_source_catalog — each source's catalog read, behind list_source_tables
src/core/connect/source_catalog.cpp          information_schema (Postgres/Snowflake/DuckDB), MySQL's aliased variant, sqlite_master, Polars' /v1/datasets
include/zetta/api/native_http_transport.hpp  install/shutdown the native client
src/core/api/native_http_transport.cpp       the native coroutine HTTPS client (zetta-api target)
tests/connect/test_connector.cpp             the boundary, the guard, and Snowflake against a fake transport
tests/connect/test_postgres_connector.cpp    the Postgres adapter against a local server, including its UTF-8 wire encoding
tests/connect/test_connection_store.cpp      the store, its boundary, and the no-leak contract
tests/connect/test_distributed_fetch.cpp     the tiling's exactly-once property + the bounded concurrent read
tests/connect/test_distributed_ingest.cpp    the cross-node ingest over a real loopback cluster
tests/connect/test_native_http_transport.cpp the native client against a real loopback server
tests/connect/test_mysql_connector.cpp       MySQL: boundary + guard, packet-level protocol, the driver vs a fake server (plaintext + real TLS), a guarded live run
tests/connect/test_lake_sql_connector.cpp    lake services: boundary, guard, request shape, typing, the 10,000-row cap, errors, the three catalogs, a guarded live run
src/core/testkit/cases_connect.cpp           cluster.connections + cluster.distributed_import — the in-app, GUI-runnable cases
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

Because the landing path is the same [`write_partitioned`](partitioned-write-path.md)
used by every other dataset, a connector-loaded table is indistinguishable
downstream from a file-loaded one: it featurizes
[compute-near-data](../compute/compute-near-data.md) exactly the same way. The
placement differs — a result set lands by
[fetch affinity](fetch-affinity-placement.md), on the node that pulled it, rather
than by the hash a locally-loaded dataset uses — and the whole cross-node flow is
[distributed-connector-ingest.md](distributed-connector-ingest.md).

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

It returns the landed dataset — `{database, table, rows, columns, partitions[],
coordinated, participants[]}` — with each partition's `owner_nodes`, so the
caller sees the placement across the cluster immediately, and `coordinated` /
`participants` say whether the fetch fanned out and over which nodes.

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

Two qualifications on that, both per-call rather than per-tool
([../api/consent.md](../api/consent.md)):

- **Consent enforcement is off unless the node was started with `--governance`**
  (#995). Off, the call executes and audits as `consent_bypassed`.
- **A loopback Postgres, MySQL, TiDB or lake service is not an external system.** The capability's
  `ConsentDescriber` reads the resolved connector: a host of `127.0.0.1` /
  `localhost` / `::1` stays on the machine, so it is not
  gated even with `--governance` on (#994). A remote source's challenge names
  that connector's real type and target — it used to describe every source as
  Snowflake.

**Safe mode** ([../api/safe-mode.md](../api/safe-mode.md), #1373): a saved
connection carries a level, `warn_writes` by default. Its reads run without a
prompt under that default. At `warn_all`, `create_dataset_from_query` and
`list_source_tables` answer `safe_mode_warning` with the exact query or catalog
statements (credential redacted) and run when repeated with its
`safe_mode_confirmation`. A statement the read-only guard refuses is refused,
never offered for confirmation.

## Catalog filters: `list_source_tables`

`list_source_tables` browses tables and optional columns through
`connect::list_source_catalog`, using either a saved connection or an inline
connector. Its `schema` and `match` filters are encoded as SQL string values;
Snowflake and MySQL require both single quotes and backslashes to be doubled.
Postgres uses explicit `E'…'` literals with the same escaping, so catalog reads
remain safe with either setting of `standard_conforming_strings`. SQLite and
DuckDB retain their ordinary SQL literal encoding.
The schema filter uses the same encoding in the table and column queries
([#1404](https://github.com/oramasearch/zetta/issues/1404)).

Snowflake's [string constant grammar](https://docs.snowflake.com/en/sql-reference/data-types-text#string-constants)
treats backslashes as escapes, so quote doubling alone cannot contain a filter.
Postgres likewise enables backslash escapes in ordinary strings when
[`standard_conforming_strings` is off](https://www.postgresql.org/docs/current/sql-syntax-lexical.html#SQL-SYNTAX-STRINGS-ESCAPE);
explicit `E'…'` literals make the encoding independent of that setting.
The Postgres adapter pins `client_encoding=UTF8` so its wire encoding matches
the JSON input, even when `PGCLIENTENCODING` requests a different encoding.
The connector's single-statement, read-only check remains a separate guard.
The request and response formats are unchanged; `%` and `_` in `match` retain
their existing LIKE wildcard behavior. Tests in `tests/connect/test_connector.cpp`
decode outgoing Snowflake filter literals and verify that quote, backslash,
escape-spelling and SQL-shaped inputs preserve both their value and the query
structure, including the column-catalog query. Postgres fake-runner tests
check both string settings. These fake tests need no warehouse or database;
the local Postgres adapter test verifies UTF-8 overrides `PGCLIENTENCODING=SJIS`.

## Connector destination approval (#1414, LSR-241)

`list_source_tables` accepts a saved connection for a reader with discovery
permission. An inline connector also requires `orchestration:create_connection`
(and, for a user identity, access to the `create_connection` tool). The same
rule applies to REST, MCP and calls made by agents through the registry.

Every connector type validates its authority before any protocol exchange,
including direct C++ calls, asynchronous submission and serial/concurrent
distributed partition reads. Each protocol adapter (Snowflake, Postgres, the
MySQL driver that also serves TiDB, and the lake SQL connector) refuses any
other connector type before transport, so a saved connection whose type changed
is validated again as its new type.

| Type | Authority | Protocol | Where the address is checked |
|---|---|---|---|
| `snowflake` | `account` is a DNS host prefix of `snowflakecomputing.com`, never a URL | HTTPS only | the native HTTP transport, every resolved address and redirect hop |
| `postgres` | `host`: one DNS name or bare IP literal; `port` 1 to 65535 (default 5432) | libpq, `sslmode` from the libpq list | resolved once on the offload worker; libpq gets only approved numeric `hostaddr` entries |
| `mysql`, `tidb` | `host`: one DNS name or bare IP literal; `port` 1 to 65535 (default 3306 / 4000) | the MySQL wire protocol; `sslmode` verify-full, verify-ca, require or disable | the MySQL driver resolves once and dials only approved numeric addresses |
| `lake_sqlite`, `lake_duckdb`, `lake_polars` | `endpoint`: `http://` or `https://` written out, one DNS name or bare IP literal, optional port | the scheme the endpoint names; an `https://` service is never followed to `http://` | the native HTTP transport, every resolved address and redirect hop |

Socket paths, host lists, authority punctuation, IPv6 zone IDs and embedded
NULs are refused for every type. A lake endpoint without a scheme is refused:
lake's services run inside the cluster on plain `http://` or on `https://`, and
the protocol the node dials is the one the connection states.

Resolved public addresses are allowed. A private or loopback address requires
an exact operator-approved hostname and port:

```sh
zetta --connector-allow-destinations db.workspace.svc.cluster.local:5432
ZETTA_CONNECTOR_ALLOW_DESTINATIONS="db.workspace.svc.cluster.local:5432,orders.ws-1.svc.cluster.local:8080"
```

The flag overrides the environment. Entries are comma/space-separated, with
`[address]:port` for IPv6. DNS comparisons ignore case and one final dot; IP
literals are canonicalized and ports compared numerically. There are no
wildcards or broad private-range grants. Invalid settings stop startup.
Metadata, link-local, reserved and multicast addresses are always denied,
including IPv4 addresses embedded in IPv6, even for an approved hostname.
Action-delivery allowances and `ZETTA_EGRESS_ALLOW=*` do not grant connector
access. Consent approval does not grant destination access either.

The HTTP transport checks every resolved candidate and every redirect hop
(after the egress allowlist, #75, which still applies). It requires HTTPS for
Snowflake and for an `https://` lake endpoint; only a lake endpoint written
with `http://` sets `HttpRequest::connector_plaintext`. Postgres resolves once
on its existing offload worker and passes only approved numeric `hostaddr`
entries to libpq, alongside the original host for TLS verification. The MySQL
driver dials the checked numeric address and still verifies the server
certificate against the configured host name (verify-full). No protocol looks
the host up again between policy approval and connection. A refusal
occurs before protocol connection and returns no peer response.

**Deployment change:** set `ZETTA_CONNECTOR_ALLOW_DESTINATIONS` on every
workspace Zetta, including every distributed-fetch participant, to the exact
service DNS names and ports of that workspace's approved databases. Use only
its own namespace's services; do not grant another workspace's service name,
a broad cluster address range, or a link-local destination. New connector
implementations must apply this policy at their dialer too; adding a connector
type does not grant network access. Existing private/loopback sources need
this setting before their next query; Unix-socket and multi-host configs must
be replaced by a single approved TCP authority. No new JSON format is added.

`test_connector_destinations.cpp` covers exact authority matching, malformed
settings, address encodings, unconditional metadata/link-local denial, and the
native HTTP transport, Postgres, MySQL, TiDB and every lake SQL type refusing an
unapproved private or loopback target before any TCP connection (a local
listener sees none) while an exact approved `host:port` connects. The registry test
covers user and AccessKey readers, saved sources and privileged inline discovery.

## No foreign runtime (ADR-17)

There is **no warehouse driver or SDK in the engine**. A connector builds plain
HTTP requests against the source's public REST API and parses JSON. Even the HTTP
exchange is injected, through the `HttpTransport` injection point:

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
in the `zetta-api` target and installs itself as the process-wide default at
startup, so
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

### The cross-node launch

The tiling is not only used *within* the coordinating node: it is what a
**cluster-wide co-launch** runs on. One node submits, fans a `run.launch` task
out over the roster, and every participant derives the same tiling, pulls only
its own tile straight from the source, and lands it as partitions **it** owns —
so the cluster's uplinks are all spent and no row crosses the wire twice.

The landing half is what used to block it, and it is now built: a
[partition-only table](partition-only-tables.md) does not capture rows into the
CRDT, and [fetch-affinity placement](fetch-affinity-placement.md) keeps each
node's slice where that node pulled it. Saved connections (below) are how each
participant presents the credential itself, so the token never rides the launch
envelope.

The whole flow — submit, launch, fetch, land, agree, and what happens when a
node dies mid-fetch — is
[distributed-connector-ingest.md](distributed-connector-ingest.md).

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

### Who may use a connection (#1339)

Holding those rights is not enough to use a particular connection: knowing a
connection's name is not being able to use it. Each saved row records its
**owner**, the user (or legacy key) that created it. `connect::may_access_connection`
is the per-connection check:

| Caller | Use the credential (`list_source_tables`, `create_dataset_from_query`) | Replace (`create_connection` on its name), delete, set its safe mode |
| --- | --- | --- |
| the connection's owner | yes | yes |
| `connections:use` flat (admin) or `ON CONNECTION <name>` | yes | no |
| `connections:manage` flat (admin) or `ON CONNECTION <name>` | only with `connections:use` too | yes |
| anyone else, including another holder of `pipelines:manage` | **refused**, `unauthorized`, audited | **refused**, `unauthorized`, audited |
| the trusted local session (no network caller) | yes | yes |

```sql
GRANT connections:use ON CONNECTION warehouse TO ROLE analysts;
GRANT connections:manage ON CONNECTION warehouse TO ROLE platform;
```

A refusal records a `data_access` audit event with `allowed: false`, the caller as
actor, `connection:<name>` as target, and the capability and missing right as
detail. Replacing a connection keeps its owner, so a manager rotating the
credential does not take it over. A connection grant names one connection exactly,
never a prefix. A row saved before the owner was recorded, or by the trusted local
session, has no owner: only a holder of the right may use it, so an existing
non-admin caller needs a grant after upgrading (`GRANT connections:use ON
CONNECTION <name> TO ROLE …`). The cross-node launch is not re-checked on peers:
the coordinator checked the caller before launching, and the launch carries only
the connection's name between mutually authenticated members.

### The credential is sealed at rest (#1230)

Until [#1230](https://github.com/oramasearch/zetta/issues/1230) the store row held
the token in plaintext, and the row — like every row — is in the replicated
catalogue every node gossips and persists, and was readable through
`SELECT * FROM zetta_connect.connections`. Now:

- **Sealed.** The row holds the settings without the token (`connector`, from
  `ConnectorConfig::to_json()`) and the token sealed with **AES-256-GCM**
  (`credential`: `{v, alg, kid, nonce, ciphertext, tag}`, the
  [`sealed_credential`](../../schemas/sealed_credential.schema.json) schema). The
  connection's name is the associated data, so a sealed value copied onto another
  row does not open.
- **The key is outside the catalogue, and outside `data_dir` (#1314).** It is
  derived (HKDF-SHA256) from the root secret `main` reads from
  `--connection-root-secret-file PATH` (or `ZETTA_CONNECTION_ROOT_SECRET_FILE`): a
  file outside the data directory, such as a mounted Kubernetes Secret, held in
  memory only. Every member is given the same file, so each opens what another
  sealed and the distributed fetch still finds the credential on every node. A
  file that resolves inside `data_dir`, is writable by group or others, or holds a
  secret shorter than 32 characters stops the node at start. Without the file the
  node holds no keyring: `create_connection` is refused, saved connections list as
  `credential: "unreadable"`, and everything else runs. (Before #1314 the key came
  from the join token in `<data_dir>/security/`, so a copy of `data_dir` opened
  every credential; the join tokens are now kept only as retired secrets, so those
  rows re-seal under the external secret at the first start that has it. The
  operator mounts the file: [orama-operator#1166](https://github.com/oramasearch/orama-operator/issues/1166).)
  A node embedded without one (the library, a test, `connection_root_secret_required`
  unset) creates a node-local secret in `<data_dir>/security/connection-root-secret`
  (0600) on first use.
  Both paths create the security directory at 0700 and secret files at 0600
  before writing; the local root loader also restricts legacy permissions and
  rejects links and special files ([storage contract](../cluster/security.md#on-disk--data_dirsecurity), #1413).
- **Rotation re-seals.** A sealed value names its key id. The root secret file's
  first line is the current secret and later lines are retired secrets the keyring
  can still open (the join token and its `retired_join_tokens` are added as
  retired too); `Node::start` runs `connect::reseal_connections`, which rewrites
  every entry not under the current key — and any legacy plaintext row — sealed
  under the current one. A steady store is not rewritten. Saving a new credential
  under an existing name seals it afresh (credential rotation).
- **A credential a node cannot open** (sealed under a key it does not hold) leaves
  the connection listable by name and settings, with `credential: "unreadable"`
  and an empty token — a connector query through it fails with that reason.

| Surface | Carries the token? |
| --- | --- |
| the store row and the replicated catalogue | only **sealed** |
| `find_connection` / `list_connections` (C++) | the **opened** token, in memory, for the connector query that presents it |
| `ConnectorConfig::to_json()` | no — cannot emit it |
| `SavedConnection::to_redacted_json()` | no — built from `to_json()`, plus `credential`: `set` / `none` / `unreadable` / `plaintext` |
| `create_connection` / `list_connections` output | no — `credential` says whether one is set |
| `system.connections` | no — a `credential` column says whether one is set |
| SQL on `zetta_connect.*` | refused for every caller, the local session included |
| audit event details, log lines, error reasons | no |

`tests/connect/test_connection_store.cpp` asserts this rather than trusting it: it
searches the stored row, the replicated catalogue, every file under the data
directory and every read surface for the secret and requires a miss, re-seals a
legacy plaintext row on start, and rotates the root secret.
`tests/connect/test_credential_seal.cpp` covers the cipher itself (fresh nonces,
wrong key, wrong connection, tampering, the local secret file).

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

This, the replicate-to-all round-trip and the no-leak contract are also an **in-app
test-suite case** —
[`cluster.connections`](../testing/runbooks/test-cases/cluster.connections.md),
runnable from Operations → Test suite and over the `testsuite_run` capability. It
asserts the refusal by *counting transport calls*: a rejected write leaves the
counter at zero, which is what "before a request is built" means in practice.

The guard a call actually gets is `connector_query_rejection(config, sql)`: the
rule above, plus the source's own dialect. **MySQL** also admits `SHOW`, treats
`#` as a comment, and refuses `/*! … */` (an *executable* comment — the server
runs its contents) and `INTO OUTFILE` / `INTO DUMPFILE` (server-side file
writes). The wire-protocol connectors then enforce read-only again at the
driver — a read-only transaction that is never committed. **lake's services**
(`lake_sqlite`, `lake_duckdb`, `lake_polars`) have no read-only mode to ask for,
so their guard also refuses every write or session word (`INSERT`, `DELETE`,
`REPLACE INTO`, `ATTACH`, `PRAGMA`, `SET`, …) anywhere outside a string, a quoted
identifier or a comment — SQLite and DuckDB accept `WITH … DELETE`.

## lake's SQLite, DuckDB and Polars services (HTTP)

lake's `sqlite`, `duckdb` and `polars` service kinds are small web services with
one shared API, `POST /v1/query {"sql"}` → `{"columns", "rows", "truncated"}`
behind a bearer token, so one connector serves three types (#1014).

| Concern | How |
| --- | --- |
| Config | `type: "lake_sqlite" \| "lake_duckdb" \| "lake_polars"`, `endpoint` (the base URL), the bearer in `token` |
| TLS | the endpoint's scheme: none means `https://` (verified against the system trust store); plaintext `http://` must be written out (lake's in-cluster services are `http://`) |
| Read-only | the shared SELECT/WITH guard plus the write/session word refusal above |
| Rows | at most 10,000 per answer (the services' cap); a truncated answer short of `max_rows` is an error, never a partial dataset |
| Typing | the services answer JSON types; `{"$base64"}` → base64 text, nested values → JSON text, decimals stay exact text |
| Catalog | SQLite: `sqlite_master` + `pragma_table_info()`; DuckDB: `information_schema`; Polars: `GET /v1/datasets` |

The full connector skill is [connectors/lake-sql.md](connectors/lake-sql.md).

## MySQL (native protocol)

MySQL has no HTTP API, so the connector speaks the **MySQL client/server
protocol** itself — natively, with no client library (#895). The protocol is a
sans-I/O `MysqlClientSession` in zetta-core; `src/core/api/mysql_protocol.cpp`
(zetta-api) drives it over a coroutine TCP socket, upgrading to TLS when the
session asks, on its own `io_context` inside `runtime::offload`.

| Concern | How |
| --- | --- |
| Config | `type: "mysql"`, `host`, `user` (required), `port` (default 3306), `database` (optional default schema), the password in `token` |
| TLS | `sslmode` — empty means `verify-full` (TLS before any credential, chain + host verified against the system trust store); `verify-ca`, `require` (encrypt only), `disable` (plaintext, by name). A server without TLS is refused unless `disable` |
| Auth | `caching_sha2_password` (fast auth; full auth over TLS only), `mysql_native_password`, `sha256_password` / `mysql_clear_password` over TLS only; AuthSwitch followed |
| Read-only | no multi-statements, no LOCAL INFILE; `SET SESSION TRANSACTION READ ONLY`, `START TRANSACTION READ ONLY`, the statement, `ROLLBACK` |
| Rows | `sql_select_limit` server-side + a client cap that stops reading and closes |
| Typing | the text protocol's column types: integers → integers, `DECIMAL`/`FLOAT`/`DOUBLE` → numbers, `BIT` → integer, binary → `0x…` hex, the rest as the server's text |

The full connector skill — least-privilege grants, catalog quirks, gotchas — is
[connectors/mysql.md](connectors/mysql.md).

**TiDB** (`type: "tidb"`, default port 4000) is the same protocol and driver.
TiDB parses `READ ONLY` but does not enforce it (with its default
`tidb_enable_noop_functions = OFF` the statements are errors), so when the
greeting names TiDB — whichever connector type reached it — the session refuses
a statement holding any write or session word (`write_words.hpp`) before the
credentials are sent, skips the READ ONLY statements, and runs the statement in a
plain `START TRANSACTION` that is rolled back. The `tidb` type's guard applies
the same word refusal before connecting. See [connectors/tidb.md](connectors/tidb.md).

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
boundary: an unknown type, a rejected statement, a transport failure, an error status
from the source, and a malformed response all come back as a
`ConnectorResult::error`.

## Scope notes / later slices

- Four connectors ship: Snowflake (HTTP), Postgres (libpqxx), MySQL and TiDB
  (native protocol) and lake's SQLite/DuckDB/Polars services (HTTP, three types). Another
  is a new branch in `run_connector_query`, an entry in `connector_types()`, and
  its catalog read in `source_catalog.cpp`.
- The **cross-node co-launch** of the fetch is a later slice — see *What is not
  wired yet* above for the landing-path decision it waits on.
- The native client does **no connection pooling**: one connection per exchange
  with `Connection: close`, so a fan-out pays one TLS handshake per request. A
  pooled variant is a change behind the same injection point.
- **The HTTPS client's TLS is not covered by a test** — the loopback suite
  exercises the same coroutine over a plain socket, and the mTLS peer transport
  covers the Corosio TLS surface separately. (The MySQL driver's TLS upgrade *is*
  tested over real TLS against a loopback server, `test_mysql_connector.cpp`.)
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
