# lake's SQLite, DuckDB and Polars services — connector skill

## Summary

lake runs managed services for each customer. Three of its service kinds are
small web services Orama wrote around an embedded engine:
[`lake-sqlite`](https://github.com/oramasearch/lake-sqlite) (SQLite),
[`lake-duckdb`](https://github.com/oramasearch/lake-duckdb) (DuckDB) and
[`lake-polars`](https://github.com/oramasearch/lake-polars) (Polars SQL over
uploaded Parquet datasets). They share one API — `POST /v1/query {"sql"}` answered
with `{"columns", "rows", "truncated"}` behind a bearer token — so one connector
reads all three, as three connector types: **`lake_sqlite`**, **`lake_duckdb`**
and **`lake_polars`** ([#1014](https://github.com/oramasearch/zetta/issues/1014)).
Pull from one when the customer's application data lives in a lake service and a
Project needs it as a dataset: `create_connection`, `list_source_tables`, then
`create_dataset_from_query`.

## Transport + auth

- **Protocol:** HTTP, through the injected `HttpTransport` (the native coroutine
  client in zetta-api). No driver and no engine is embedded in zetta: the engine
  runs inside the service. The wire details are
  `src/core/connect/lake_sql_protocol.hpp/.cpp`.
- **Config:** `type`, `endpoint` (the service's base URL) and the bearer in
  `token`. lake delivers both in the service's credential (`<slug>.json`:
  `endpoint`, `token`); its `connect_service` maps them
  ([lake#223](https://github.com/oramasearch/lake/issues/223)).

  ```json
  {"type": "lake_duckdb",
   "endpoint": "http://orders.ws-1.svc.cluster.local:8080",
   "token": "<the service's bearer token>"}
  ```

- **TLS:** the endpoint's scheme. An endpoint **without a scheme is `https://`**,
  verified against the system trust store by the native client. Plaintext
  `http://` must be written out. lake's services listen on plain HTTP inside the
  cluster today (`http://<name>.<ns>.svc.cluster.local:8080`), so a connection
  lake makes names `http://` and its token crosses the cluster network
  unencrypted; the services are not reachable from outside the cluster.
- **Credentials:** the bearer token rides only in `token`: sealed when saved with
  `create_connection`, sent as `Authorization: Bearer …` and marked secret on the
  request, never echoed by `list_connections`, never in an error. An endpoint
  carrying user info (`http://user:pass@…`), a query or a fragment is refused, so
  a credential cannot hide in the URL.
- **Consent:** an endpoint on `127.0.0.1`, `localhost` or `::1` is local and runs
  ungated; any other host is a network reach, and the challenge names it
  (`lake_duckdb http://orders…:8080`).

## Read semantics

- **Read-only, two ways.** (1) The connector guard (`connector_query_rejection`)
  admits one `SELECT` or `WITH` statement, as for every connector. (2) Because
  the services run what they are sent — lake-sqlite runs every `;`-separated
  statement, and SQLite and DuckDB accept `WITH … INSERT/UPDATE/DELETE` — the
  guard also refuses any word that starts a write or changes the session
  (`INSERT`, `UPDATE`, `DELETE`, `REPLACE INTO`, `CREATE`, `DROP`, `ALTER`,
  `ATTACH`, `PRAGMA`, `COPY`, `INSTALL`, `LOAD`, `SET`, `BEGIN`, `COMMIT`, …)
  wherever it appears outside a `'…'` string, a `"…"` identifier or a comment
  (`src/core/connect/write_words.hpp`). Only those two quote forms are skipped
  (all three engines share them), and the statement is scanned both with and
  without backslash escapes (DuckDB's `E'…'` strings take them), so a quote the
  scan reads differently from the engine can only refuse more; a column
  that happens to be named `load` or `set` must be quoted (`"load"`). The service
  is not asked to enforce anything further: none of the three has a read-only
  request mode. lake-duckdb's own lockdown also refuses file, network and
  extension access, and lake-polars' sandbox admits only queries over its
  datasets.
- **Row cap:** each service returns **at most 10,000 rows** per query and marks a
  longer result `"truncated": true`. `max_rows` caps on the client. When a result
  is truncated and `max_rows` asks for more than arrived, the query **fails**
  rather than landing a silently partial dataset: narrow the query, or set
  `max_rows` to 10,000 or fewer to take the first rows. Paging by
  `LIMIT`/`OFFSET` is deliberately not done: without a total order it skips and
  repeats rows. Pulling more than 10,000 rows needs a paged or streamed answer
  from the services: [#1382](https://github.com/oramasearch/zetta/issues/1382).
- **Partitioning:** none. One answer per query, so ingest takes the single-shot
  path (`run_single_shot_connector_ingest`), landing the same dataset shape as
  every other connector.
- **Timeouts:** the services stop a statement at 30 s (`504 timeout`); the client
  waits up to 60 s for the answer.
- **Catalog** (`list_source_tables`, `connect/source_catalog.hpp`):
  - `lake_sqlite` reads `sqlite_master` and the `pragma_table_info()` table
    function: schema `main`, tables and views, internal `sqlite_%` tables left out;
  - `lake_duckdb` reads `information_schema.tables` / `.columns` like Postgres;
  - `lake_polars` has no SQL catalog (its sandbox admits only queries), so it
    reads `GET /v1/datasets`: one entry per dataset, kind `DATASET`, an empty
    schema (a dataset is queried by its bare name), its columns with Polars' type
    names (`i64`, `f64`, `str`, `date`, …).

## Key objects

Whatever the customer's application keeps in the service: tables and views in
SQLite or DuckDB, uploaded datasets in Polars.

Typed cells — the services answer real JSON types, and they pass through:

| Engine value | Lands as |
| --- | --- |
| integers | JSON integers |
| `REAL` / `DOUBLE` / `FLOAT` | numbers (infinities and NaN arrive as the strings `"Infinity"`, `"NaN"`) |
| text, dates, times, timestamps, UUIDs | text, in each service's format |
| booleans | `true` / `false` (SQLite has no boolean: `0` / `1`) |
| `NULL` | null |
| `BLOB` / Binary (`{"$base64": …}`) | the base64 text |
| DuckDB `DECIMAL`, Polars `Decimal` / `Int128` | **exact text** — `CAST(x AS DOUBLE)` in the query to land a number |
| DuckDB `LIST`, `STRUCT`, `MAP`, `UNION`, `INTERVAL` | their JSON text |

## Join keys

The application's own ids (`customer_id`, `order_id`, `account_id`) to the same
ids in a warehouse, CRM or billing system; email addresses to CRM contacts.

## Gotchas

- **10,000 rows.** A pull larger than that fails with the reason; aggregate in
  the query, filter, or take the first rows with `max_rows`.
- **DuckDB decimals are text.** `SELECT 2.5` is a `DECIMAL` to DuckDB, answered
  as `"2.5"`; cast to `DOUBLE` for a numeric column.
- **SQLite keywords as aliases.** `nothing`, `do`, `filter` and others are SQLite
  keywords; an alias with such a name fails in the service (`sql_error`).
- **SQLite's `INTEGER PRIMARY KEY`** reports nullable in `pragma_table_info`,
  though it can never be null.
- **A 401 can surface as a reset.** The services answer a wrong token with `401`
  and close the connection without reading the body; on some runs the client sees
  the reset before the answer and reports *could not reach* instead of *refused
  the token* ([#1383](https://github.com/oramasearch/zetta/issues/1383)).
- **Polars datasets are per worker** in lake v1: with several workers each holds
  its own datasets, and the service lists and queries whichever worker answered.

## Status

**shipped** — the connector, its read-only guard, the catalog for all three, and
consent, unit-tested against a fake transport
(`tests/connect/test_lake_sql_connector.cpp`) and run live against a local
`lake-sqlite`, `lake-duckdb` and `lake-polars` built from their repositories (the guarded
`LakeSqlLive` test: `ZETTA_LAKE_SQL_TEST_ENDPOINT`, `_TOKEN`, `_TYPE`, `_TABLE`),
plus an end-to-end pull through a running node's REST API. lake's side — mapping a
service's credential into `create_connection` and lifting the kinds on the Data
page — is [lake#223](https://github.com/oramasearch/lake/issues/223).
