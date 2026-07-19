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
include/zetta/connect/connector.hpp        ConnectorConfig / ConnectorRows / run_connector_query
include/zetta/connect/http_transport.hpp   HttpRequest / HttpResponse / HttpTransport seam
src/core/connect/connector.cpp             the config boundary, the read-only guard, the Snowflake connector
src/core/connect/http_transport.cpp        the default transport (curl via CommandRunner)
tests/connect/test_connector.cpp           the boundary, the guard, and Snowflake against a fake transport
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

## No foreign runtime (ADR-17)

There is **no warehouse driver or SDK in the engine**. A connector builds plain
HTTP requests against the source's public REST API and parses JSON. Even the HTTP
exchange is injected, through the `HttpTransport` seam:

```cpp
using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;
```

This is the same **injected-transport** pattern as the
[`CommandRunner`](../cloud/provisioners.md) behind the CLI cloud control planes: it
keeps connectors unit-testable against a fake (no network, no credentials, no
warehouse — every case in `tests/connect/test_connector.cpp` runs that way), and
keeps the real egress path in one replaceable place.

The default transport shells **`curl`** through the shared `CommandRunner`. A
native, coroutine-backed transport behind `zetta::runtime::net` can replace it
later without touching a single connector, since both sides only ever exchange
`HttpRequest` / `HttpResponse`.

### Credentials never ride in argv

`HttpRequest::secret_headers` names the headers whose values are secret (the
`Authorization` bearer). The default transport writes exactly those to a private
**0600 temp config file** consumed via `curl --config`, so a token is never
visible in the process table. The request body goes to its own file too, so a
large statement cannot hit the argv size limit. Both are removed after the
exchange. `ConnectorConfig::to_json()` likewise **never emits the token**, so a
config is safe to echo back to a caller or log.

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
- The **native HTTP transport** (coroutine-backed, behind `zetta::runtime::net`)
  replaces the curl default behind the same seam.
- Async polling waits on the **caller's thread** — this is a synchronous data-load
  action, like `add_dataset`.

See also [add-dataset.md](add-dataset.md) (the landing path),
[partitioned-write-path.md](partitioned-write-path.md) (how rows spread across the
cluster), and [ddf-featurize.md](../ddf/ddf-featurize.md) (what you do with the
dataset once it lands).
