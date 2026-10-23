# TiDB — connector skill

## Summary

TiDB is a distributed SQL database that speaks the MySQL protocol. lake offers
it as its `tidb` service kind (PingCAP TiDB Operator, one `TidbCluster` per
service). Pull from it when an application's data lives there: connect with
connector type **`tidb`**, then use `list_source_tables` and
`create_dataset_from_query` ([#1014](https://github.com/oramasearch/zetta/issues/1014)).
TiDB goes through the same native MySQL protocol and driver as
[MySQL](mysql.md) (#895). Only the read-only fencing differs, because TiDB does
not enforce `READ ONLY`.

## Transport + auth

- **Protocol:** the MySQL client/server protocol, implemented natively
  (`include/zetta/connect/mysql_client.hpp`, driver
  `src/core/api/mysql_protocol.cpp`). The default port is **4000**, TiDB's SQL
  port.
- **Config:** `type: "tidb"`, `host`, `user` (required), `port` (default 4000),
  `database` (optional default schema), the password in `token`. lake's
  credential carries `host`, `port`, `user` (`root`), `password`.

  ```json
  {"type": "tidb", "host": "orders-tidb.ws-1.svc.cluster.local", "port": "4000",
   "user": "root", "token": "<password>", "sslmode": "disable"}
  ```

- **TLS:** `sslmode`, exactly as for MySQL. The default (empty) is `verify-full`:
  TLS before any credential, the chain and host verified. **TiDB offers no TLS
  unless it is configured** (certificates, or `security.auto-tls`), and the
  default refuses such a server before the password is sent. lake's TiDB has no
  client TLS today, so a lake connection names `sslmode: "disable"`. Its password
  then crosses the cluster network in the clear, though the service is not
  reachable from outside the cluster.
- **Destination:** the same check as MySQL (#1414): one DNS name or IP literal,
  a numeric port, and only addresses the connector destination policy approves.
  lake's TiDB is a private cluster address, so the operator lists its exact
  `host:port` (`orders-tidb.ws-1.svc.cluster.local:4000`) in
  `ZETTA_CONNECTOR_ALLOW_DESTINATIONS`.
- **Authentication:** TiDB's default plugin is `mysql_native_password`;
  `caching_sha2_password` works as for MySQL (full authentication only over TLS).
- **Credentials:** sealed when saved with `create_connection`, never echoed,
  never in an error.

## Read semantics

- **Why TiDB is fenced differently.** TiDB parses `READ ONLY` but does not
  enforce it. With `tidb_enable_noop_functions` OFF (its default),
  `SET SESSION TRANSACTION READ ONLY` and `START TRANSACTION READ ONLY` are
  errors (*function READ ONLY has only noop implementation*). With it ON they are
  accepted and ignored, and the transaction stays read-write (TiDB's own docs on
  `tidb_enable_noop_functions` and `START TRANSACTION`). MySQL's fencing would
  therefore fail on every query, or silently fence nothing.
- **Read-only, four ways, for TiDB:**
  1. The guard admits one `SELECT`, `WITH` or `SHOW` statement, with MySQL's
     dialect rules: `#` comments, no `/*! … */`, no `INTO OUTFILE` / `DUMPFILE`.
  2. On top of that, the guard refuses any word that writes, changes the schema,
     or changes the session or transaction (`INSERT`, `UPDATE`, `DELETE`,
     `REPLACE INTO`, `LOAD`, `CREATE`, `DROP`, `ALTER`, `SET`, `LOCK`, `HANDLER`,
     `PREPARE`, `FLASHBACK`, `SPLIT`, `ADMIN`, …). It looks everywhere outside
     strings, quoted identifiers and comments, scanning with and without
     backslash escapes (`src/core/connect/write_words.hpp`). This also refuses
     `SELECT … FOR UPDATE`.
  3. The session recognises TiDB by its greeting (`8.0.11-TiDB-v7.5.1`). It
     applies the same word refusal **before any credential is sent**, whatever
     the connector type: a `mysql` connection that reaches a TiDB is fenced as
     TiDB too.
  4. The session never offers multi-statements or LOCAL INFILE. It runs the
     statement inside a plain `START TRANSACTION`, which is rolled back and never
     committed, so a write the guard could not recognise is undone.

  DDL would commit implicitly in TiDB, which is why the guard refuses every DDL
  word rather than relying on the rollback.
- **Row cap:** `SET SESSION sql_select_limit = N` (supported by TiDB) plus the
  client cap, which stops reading and closes the connection.
- **Snapshot:** TiDB's `START TRANSACTION` takes a consistent snapshot at once
  (MySQL's `WITH CONSISTENT SNAPSHOT`), so the rows come from one point in time.
- **Partitioning:** none; one result stream, the single-shot ingest path.
- **Catalog:** `information_schema.tables` / `.columns`, read as for MySQL
  (every column aliased, backslashes doubled in literals). TiDB names its system
  schemas in upper case under a binary collation, so the schema is lower-cased
  before `information_schema`, `mysql`, `performance_schema`, `sys` and
  `metrics_schema` are excluded.

## Key objects

Whatever the application keeps, as with MySQL: accounts, orders, subscriptions,
events.

Typed cells as for MySQL: integers → integers (`BIGINT UNSIGNED` above
`INT64_MAX` stays unsigned), `DECIMAL` / `FLOAT` / `DOUBLE` → numbers, binary →
`0x…` hex text, dates, times, `JSON`, `ENUM` and `SET` → the server's text.

## Join keys

The application's own ids to the same ids in a warehouse, CRM or billing system,
and email addresses to CRM contacts.

## Gotchas

- **`sslmode` must be named for lake's TiDB** (`disable`): the default refuses a
  server without TLS.
- **A column named like a write word** (`load`, `lock`, `admin`, `split`) has to
  be quoted with backticks to pass the guard.
- **`SELECT … FOR UPDATE` and `LOCK IN SHARE MODE`** are refused: they take
  locks, and a read has no need of them.
- **`tidb_enable_noop_functions = ON`** on the server does not weaken the
  connector: it never sends `READ ONLY` to TiDB.
- **Stale reads** (`AS OF TIMESTAMP`) are not used; a pull reads current data.

## Status

**shipped** — the same native protocol and driver as MySQL, with the session
switching to TiDB's fencing from the greeting. It is unit-tested against a
scripted TiDB greeting (the fencing, and the refusal before credentials), and the
driver is tested over loopback against a fake server that greets as TiDB and
refuses `READ ONLY` as TiDB does, whichever connector type named it. **A run
against a real TiDB is owed** (none on the build host): it is the guarded
`TidbLive` test (`ZETTA_TIDB_TEST_HOST`, `_PORT`, `_USER`, `_PASSWORD`, `_DB`,
`_SSLMODE`), tracked with MySQL's live run in
[#1368](https://github.com/oramasearch/zetta/issues/1368). lake's side is
[lake#223](https://github.com/oramasearch/lake/issues/223).
