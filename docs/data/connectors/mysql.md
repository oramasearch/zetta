# MySQL — connector skill

## Summary

MySQL (and the protocol-compatible MariaDB, Percona Server, Amazon RDS/Aurora
MySQL, Azure Database for MySQL, Google Cloud SQL for MySQL, PlanetScale) is the
most common operational database behind line-of-business applications: orders,
accounts, subscriptions, tickets. Pull it when the facts a Project needs live in
an application's own database rather than a warehouse. Zetta connects natively,
runs one read-only statement, and lands the rows as a partitioned dataset through
`create_dataset_from_query` ([#895](https://github.com/oramasearch/zetta/issues/895)).

## Transport + auth

- **Protocol:** the MySQL client/server protocol over TCP (default port 3306).
  There is no HTTP + SQL API, so this is a wire-protocol connector. Zetta
  **implements the protocol natively** — no libmysqlclient, no MariaDB
  Connector/C — so it needs no ADR-17 exception: the protocol lives in zetta-core
  as a sans-I/O state machine (`include/zetta/connect/mysql_client.hpp`), and a
  coroutine TCP/TLS driver in zetta-api (`src/core/api/mysql_protocol.cpp`) moves
  its bytes. Spoken: HandshakeV10 (MySQL 4.1+ / every MariaDB), the text result
  protocol of `COM_QUERY`.
- **TLS:** the connector's `sslmode`. The default (empty) is **`verify-full`**:
  the connection is upgraded to TLS (the protocol's SSLRequest, then the TLS
  handshake) **before** any credential is sent; the server certificate must chain
  to the system trust store (`SSL_CERT_FILE`, then the platform bundle) and match
  the `host`. `verify-ca` checks the chain but not the name; `require` encrypts
  without verifying (a self-signed server — the default for a stock MySQL
  install); `disable` is plaintext and must be named. A server that does not offer
  TLS is refused before the password leaves the process. MySQL's own
  `--ssl-mode` spellings (`REQUIRED`, `VERIFY_CA`, `VERIFY_IDENTITY`, `DISABLED`)
  are accepted; `PREFERRED` is not, because it falls back to plaintext silently.
- **Destination:** `host` is one DNS name or bare IP literal and `port` a number
  from 1 to 65535; socket paths, host lists and URL punctuation are refused
  (#1414). The driver resolves the host once and dials only addresses the
  connector destination policy approves: public addresses, or a private or
  loopback one whose exact `host:port` the operator lists in
  `ZETTA_CONNECTOR_ALLOW_DESTINATIONS`. Metadata, link-local and reserved
  addresses never pass. It connects to the checked numeric address, so a second
  DNS answer cannot redirect it, and `verify-full` still matches the certificate
  to `host`. See [../connector-query.md](../connector-query.md#connector-destination-approval-1414-lsr-241).
- **Credentials:** `user` + the password in `token` — saved once with
  `create_connection` (sealed, replicated, never echoed) and referenced by name.
  The password is never logged and never appears in an error.
- **Authentication plugins:** `caching_sha2_password` (the MySQL 8 default —
  fast authentication by scramble; *full* authentication sends the password and
  is only done over TLS), `mysql_native_password` (SHA-1 scramble; MySQL 5.x and
  MariaDB), and `sha256_password` / `mysql_clear_password` over TLS only. The
  server may switch plugins mid-handshake (AuthSwitchRequest); that is followed.
  Not implemented: the RSA public-key exchange that lets `caching_sha2_password`
  full authentication run without TLS — use TLS.
- **Least privilege:** a dedicated account with `SELECT` (and, for the catalog,
  nothing more) on the schemas it should read, `REQUIRE SSL`, and no `FILE`,
  `PROCESS`, `SUPER` or write grants:

  ```sql
  CREATE USER 'zetta_reader'@'%' IDENTIFIED BY '…' REQUIRE SSL;
  GRANT SELECT ON shop.* TO 'zetta_reader'@'%';
  ```

## Read semantics

- **Read-only, three ways.** (1) The connector guard
  (`connector_query_rejection`) admits one `SELECT`, `WITH` or `SHOW` statement;
  it treats `#` as a comment, and refuses MySQL executable comments (`/*! … */`,
  which the server runs as SQL) and `INTO OUTFILE` / `INTO DUMPFILE` (which write
  files on the server and are not stopped by a read-only transaction). (2) The
  driver never offers `CLIENT_MULTI_STATEMENTS` or `CLIENT_LOCAL_FILES`, and
  refuses a server's LOCAL INFILE request. (3) Before the statement runs the
  session is fenced: `SET SESSION TRANSACTION READ ONLY`, then
  `START TRANSACTION READ ONLY`; the transaction is rolled back, never committed.
  A write the guard cannot see (MySQL 8's `WITH … DELETE`) fails with error 1792.
- **Row cap:** `max_rows` is applied server-side (`SET SESSION sql_select_limit`)
  and client-side; at the cap the driver stops reading and closes the connection,
  which ends the transaction on the server.
- **Partitioning:** none. A MySQL result is one stream, so ingest takes the
  single-shot path (`run_single_shot_connector_ingest`) to the same dataset shape
  Snowflake's tiled path produces. A keyed range split pushed into the query is a
  later slice.
- **Rate limits:** none at the protocol; `max_connections` and per-account
  `MAX_USER_CONNECTIONS` are the practical bound. One query opens one connection.
- **Timeouts:** 10 s to connect, 15 s for the TLS handshake, 300 s of silence on
  a read.
- **Catalog:** `list_source_tables` reads `information_schema.tables` /
  `.columns`, aliasing every column (MySQL 8 reports them in upper case) and
  excluding `information_schema`, `mysql`, `performance_schema` and `sys`.

## Key objects

Whatever the application keeps. Typical operational schemas: `customers` /
`accounts` (id, email, created_at, plan), `orders` and `order_items` (customer
id, amounts, status, timestamps), `subscriptions` (status, renewal dates,
cancellation reason), `tickets` / `events` (customer id, type, created_at).

Typed cells: integer types (incl. `YEAR`) → JSON integers (`BIGINT UNSIGNED`
above `INT64_MAX` stays an unsigned integer); `DECIMAL`, `FLOAT`, `DOUBLE` →
numbers; `BIT` → its integer value; `NULL` → null; binary strings and blobs →
`0x…` hex text; dates, times, `JSON`, `ENUM` and `SET` → the server's own text.

## Join keys

Application primary keys (`customer_id`, `account_id`) to the same ids in a
warehouse or CRM; `email` (and its domain) to CRM contacts and support systems;
order and subscription ids to billing (Stripe metadata often carries them).

## Gotchas

- **`caching_sha2_password` without TLS** fails on the first connection after a
  server restart (the server's cache is empty, so it asks for full
  authentication). Use TLS — the default — and it works.
- **Stock MySQL certificates are self-signed** (auto-generated at install), so
  `verify-full` fails against them; use `require`, or give the server a
  certificate that chains to a trusted CA. A `host` given as an IP address will
  not match a certificate issued for a name.
- **Duplicate column names** in a result (two `id`s from a join): the later one
  wins in the row object — alias them.
- **Zero dates** (`0000-00-00`) arrive as that text, not as a date.
- **`sql_select_limit`** caps only the top-level `SELECT`; `SHOW` is capped by the
  client.
- **MariaDB** is spoken through the same protocol; its `ed25519` and GSSAPI
  plugins are not implemented.
- **TiDB** is spoken through the same protocol (connector type `tidb`), but it
  does not enforce `READ ONLY`, so the session recognises it from its greeting
  and fences it differently — even when the connector type is `mysql`. See
  [tidb.md](tidb.md).

## Status

**shipped** — native protocol, TLS by default, read-only at the driver, unit- and
loopback-tested (including real TLS). Tracking issue:
[#895](https://github.com/oramasearch/zetta/issues/895). A run against a live
MySQL server is the guarded `MysqlLive` test (`ZETTA_MYSQL_TEST_HOST`, …).
