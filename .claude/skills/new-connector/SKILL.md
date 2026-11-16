---
name: new-connector
description: Add a NEW source connector to zetta so a query against an external system lands as a partitioned, featurizable dataset. Use when the user wants zetta to pull data from a warehouse, database, or API it doesn't speak yet — Postgres, BigQuery, Databricks, Redshift, Salesforce, S3, a REST endpoint — or says "connect to X", "load my data from X", "we need an X connector". Snowflake is the reference implementation. Never link a vendor SDK or driver.
---

# new-connector — teach zetta to pull from a new source

A **source connector** runs a read-only query against an external system and
returns rows; [`data::add_dataset_rows`](../../../docs/data/add-dataset.md) shards and
places those rows across the cluster. Together they turn a warehouse table into a
clustered, featurizable zetta dataset with no manual export.

Follow the **zetta-dev** skill for all coding conventions while doing this, and
**api-dev** when you get to the capability surface.

**Start from the practice:** [`docs/data/adding-a-connector.md`](../../../docs/data/adding-a-connector.md)
is the end-to-end method — **research** the source with a Perplexity deep-dive
(the **`perplexity`** skill) into a connector-skill doc, **decide the transport**
(HTTP+SQL API vs a wire-protocol driver — the ADR-17 gate), then **build** with
this skill. Snowflake (HTTP API), MySQL (a wire protocol implemented natively: a
sans-I/O session in zetta-core + a coroutine TCP/TLS driver in zetta-api)
and Postgres (libpqxx driver, coroutine-wrapped via `runtime::offload`, a
sanctioned ADR-17 exception) are the three worked examples. Read [`docs/data/connector-query.md`](../../../docs/data/connector-query.md)
too — it is the shipped design this skill operationalizes.

## Core principle: produce rows, nothing else

**The connector's only job is to produce rows. Zetta owns the distribution.**

Do not shard, place, replicate, or write files. Hand back
`ConnectorRows{columns, rows}` and let `add_dataset_rows` run the existing
`write_partitioned` + `RendezvousPlacement` path. This is what makes a
connector-loaded table indistinguishable downstream from a file-loaded one — it
featurizes compute-near-data exactly the same way.

The second half of the principle: **no foreign runtime (ADR-17)**. Never link the
vendor's SDK, driver, or client library. A connector builds plain HTTP requests
against the source's public REST API and parses JSON. If the source has *no* HTTP
API and truly requires a wire protocol, implement the protocol natively the way
MySQL is (`include/zetta/connect/mysql_client.hpp`) — never reach for the
vendor's client library without raising it first.

## The five non-negotiables

Every connector must satisfy all five. They are structural, not aspirational —
each one has a test in `tests/connect/`.

1. **Read-only by construction.** Only a single `SELECT`/`WITH` may run.
   `read_only_query_rejection()` already enforces this centrally, *before a
   request is built* — so a write never reaches the network. Do not bypass it,
   and do not add a "trusted" escape hatch.
2. **Credentials never leak.** The bearer goes in `HttpRequest::secret_headers`.
   No secret may appear in a reason string, log line, span attribute, or process
   argument. `ConnectorConfig::to_json()` structurally cannot emit the token —
   keep any new field that holds a credential out of it too.
3. **Typed cells, not strings.** Most REST APIs hand back every value as a JSON
   string. If you pass those through, the landed dataset is all strings and
   **nothing downstream can featurize or train on it**. Coerce using the source's
   own declared column types (see `cell()` in `snowflake_protocol.hpp`).
4. **Transport is injected.** Never open a socket in a connector. Build an
   `HttpRequest`, hand it to the `HttpTransport` / `AsyncHttpTransport` the caller
   supplied. This is what makes every connector testable with **no network, no
   credentials, and no live account**.
5. **Distributed-first.** If the source can return the result set as
   independently-fetchable chunks, expose that — it is what lets the cluster use
   its aggregate bandwidth instead of one node's link. See *Distributed fetch*
   below.

## Steps

1. **Find the source's REST query API and its result shape.** You need: how to
   submit a statement, how results come back, whether large results are chunked
   (and how to fetch chunk *i*), how async/long-running statements are signalled,
   and what column type metadata is available. Write this down before coding.

2. **Extend `ConnectorConfig` if the source needs fields Snowflake doesn't.**
   `include/zetta/connect/connector.hpp`. Keep additions optional and update
   `connector_config_schema()` in the same edit — the schema is the code-owned
   source of truth. Any credential-bearing field must stay out of `to_json()`.

3. **Add the type to `connector_types()`** (`src/core/connect/connector.cpp`).
   This one list drives the config boundary's validation *and* the enum the UI
   renders as a dropdown.

4. **Write `src/core/connect/<vendor>_protocol.hpp/.cpp`** — an **internal**
   header (nothing outside `src/core/connect/` includes it), holding pure
   request-building and response-reading: no I/O, no transport. Mirror
   `snowflake_protocol.hpp`: the endpoint URL, the headers, the per-chunk request,
   cell typing, and the column/row readers. Keeping it I/O-free is what lets the
   single-node and distributed paths share identical framing.

5. **Add the dispatch branch** in `run_connector_query`
   (`src/core/connect/connector.cpp`, near `if (config.type == "snowflake")`).

6. **Wire the distributed fetch** if the source chunks results — see below.

7. **Test against a fake transport.** Extend `tests/connect/test_connector.cpp`.
   Cover: the request shape (URL, headers, secret marking, body), typed cells
   (number/bool/null), an error status carrying the source's message, a transport
   failure, a malformed response, `max_rows` capping, and a write statement
   **never reaching the transport**. Every case runs offline.

8. **Docs in the same commit** — `docs/data/connector-query.md` (the connector
   table), the `docs/data/README.md` index if you added a page, and
   `docs/architecture/code-map.md` for every new file.

## Distributed fetch — use the cluster, not one node

If the source returns a handle plus N independently-fetchable result chunks
(Snowflake's `resultSetMetaData.partitionInfo[]` +
`GET /statements/{handle}?partition=i`), wire it into
`include/zetta/connect/distributed_fetch.hpp`:

- `plan_partition_assignments(roster, rowCounts)` tiles the chunks —
  **deterministic, disjoint, total, and balanced by row count** (longest-first),
  not round-robin over indices. Uneven chunks round-robined leave one node holding
  the tail and the whole ingest waits on that node's link.
- Because the tiling is a pure function of (roster, row counts), every node
  computes the identical plan — no plan travels on the wire.
- `fetch_result_partitions` reads a node's slice concurrently in bounded waves
  under `runtime::when_all`, each request under `runtime::timeout`. Bound the
  in-flight count: TLS handshakes with full chain verification are not free.

If the source returns one undivided document, say so explicitly in the docs and
take the serial path. Do not fake chunking by issuing `LIMIT`/`OFFSET` queries —
that re-runs the query per page and can silently skew or duplicate rows.

Cross-node co-launch is deliberately not wired yet: it is blocked on a
non-capturing partition-write path; the tiling is the piece it will fan out over.

## The capability surface

A connector reachable only from C++ is not shipped. `create_dataset_from_query`
already dispatches on `connector.type`, so a new connector usually needs **no new
capability** — it appears in MCP, REST, OpenAPI, and the web dialog the moment
`connector_types()` lists it.

If you *do* add a capability (`src/core/api/capabilities.cpp`), these break
loudly and are easy to forget:

- `registry.size()` in `tests/api/test_registry.cpp`
- `permission_operations("pipelines:manage").size()` in `tests/governance/test_roles.cpp`
- the operation must be registered in `src/core/governance/permission.cpp`, or
  **no role can grant it**
- regenerate the OpenAPI doc, then re-run to confirm clean:
  ```bash
  ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests --gtest_filter='OpenApi*'
  ./build/tests/zetta-tests --gtest_filter='OpenApi*'
  ```
- anything reaching an external system with credentials is **consent-gated**
  (`requires_consent = true`) — unlike `add_dataset`, it must not run silently

## Where things go

```
include/zetta/connect/connector.hpp            ConnectorConfig / ConnectorRows / run_connector_query
include/zetta/connect/http_transport.hpp       HttpRequest / HttpResponse / the injected transport
include/zetta/connect/distributed_fetch.hpp    result-set tiling + concurrent chunk read
include/zetta/connect/connection_store.hpp     SavedConnection — the replicate-to-all credential store
src/core/connect/connector.cpp                 config boundary, read-only guard, per-vendor dispatch
src/core/connect/<vendor>_protocol.hpp/.cpp    YOUR wire details (internal; no I/O)
src/core/api/native_http_transport.cpp         the native coroutine HTTPS client (zetta-api target)
tests/connect/test_connector.cpp               YOUR tests, against a fake transport
```

New source files go in `CMakeLists.txt` and new test files in
`tests/CMakeLists.txt` — **both are explicit lists, not globs**. A missing entry
shows up as an undefined-symbol link error, not a compile error.

## Conventions

- Non-throwing boundaries: return `ConnectorResult` with a path-qualified
  `error`; parse with
  `nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false)` and check
  `is_discarded()`. Nothing throws across a zetta-core public boundary.
- Never name `boost::capy` / `boost::corosio` / `openssl` outside the runtime
  headers and the two adapter TUs that are allowed to. `zetta-core` links no HTTP library — that is why the real client lives in
  the `zetta-api` target and installs itself as the transport at startup.
- The repo's IDE/LSP diagnostics are **false positives** (the IDE clang does not
  understand `-std=c++26`). Only `cmake --build build` is authoritative.

## Pitfalls

- **Passing strings through.** The most common way a connector "works" and is
  still useless — the dataset lands, then nothing can train on it.
- **Round-robin tiling.** Correct-looking, and it wastes most of the speedup.
- **Paging with `LIMIT`/`OFFSET`.** Re-runs the query per page; without a total
  order it silently duplicates and drops rows.
- **A token in an error string.** Build failure reasons from host/port/status and
  the transport error only — never from a header or the config.
- **Opening a socket in the connector.** Instantly untestable offline, and it
  bypasses the injection point that lets the transport be swapped.
