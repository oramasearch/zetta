# Safe mode, per database

A level set on each database that says which statements need the caller's
confirmation before they run. Modelled on Bobby Tables' safe mode. Chosen in
place of turning the consent gate on by default
([#1373](https://github.com/oramasearch/zetta/pull/1373),
[lake#906](https://github.com/oramasearch/lake/issues/906)):

> *"you can have safe mode off, warn writes, warn all ... let's do the same so we
> can warn with the queries that we're going to run, and can control it / change
> it per database."*

## The levels

| Level | What warns |
|---|---|
| `off` | nothing; every statement runs |
| `warn_writes` | a statement that writes |
| `warn_all` | every statement |

"Database" means two things, and each has its own level:

| Target | What it is | Default |
|---|---|---|
| **connection** | a saved source connection (`create_connection`: Snowflake, Postgres, MySQL, TiDB, lake's services) | `warn_writes` |
| **database** | a Zetta SQL database | `off` |

**Why a connection defaults to `warn_writes`:** that is the decision. Sources are
read through read-only guards, so their reads run without a prompt under it.

**Why a Zetta database defaults to `off`, not `warn_writes`:** Lake's control
plane (`ZettaControlClient`) and the Orama operator write their own tables
through `/v1/sql` (`orama_control`, `orama_data`). A default that refused every
write until a person confirmed it would stop both the moment a node upgraded. A
database is moved to `warn_writes` or `warn_all` with `set_safe_mode`. This is a
deliberate difference from the decision's text, recorded on #1373 to be
confirmed or reversed.

## The warning

A call that would run a statement the level warns on does **not** run it. It
fails with code `safe_mode_warning` (REST status 428), and `error.details` says
exactly what was about to run:

```json
{
  "code": "safe_mode_warning",
  "message": "'sql' did NOT run: database 'sales' is in safe mode warn_writes, ...",
  "details": {
    "database": "sales",
    "mode": "warn_writes",
    "statements": ["DELETE FROM sales.accounts WHERE closed = true"],
    "confirmation": "9f2c…64 hex digits"
  }
}
```

A connection's warning names it with `"connection": "<name>"` instead of
`"database"`. Statements are shown **with the connection's credential
redacted** (`[redacted]` wherever it appears). A multi-statement SQL input that
warns on more than one database names the first in `database` and the rest in
`also: [{database, statements}]`; the one confirmation covers all of them.

## The confirmation

Repeat **the same call** with `"safe_mode_confirmation": "<confirmation>"`. It
runs, and is audited as `safe_mode_confirmed`.

- The confirmation is a SHA-256 over the target kind, the database name and the
  statements (`governance::safe_mode_confirmation`). It confirms exactly those
  statements against that database. A confirmation for a different statement or
  database is refused with a fresh `safe_mode_warning` whose message says so.
- It is a confirmation by the person running the statement, **not a second
  approver**. That is consent ([consent.md](consent.md)), which stays opt-in per
  node (`--governance`).
- It is not remembered. The next call of a warned statement warns again.
- A confirmation given where nothing warns is ignored.

## Which statements, and which writes

| Capability | What it runs | Write? |
|---|---|---|
| `sql` | the input statement(s), verbatim, against each database they name (a table's database, or `database` for an unqualified name) | the parser's statement kind: `SELECT` reads; `CREATE`/`DROP DATABASE`, `CREATE`/`DROP TABLE`, `INSERT`, `UPDATE`, `DELETE`, `TRUNCATE` and `CREATE POLICY` write |
| `create_dataset_from_query` | the caller's query against the saved `connection` | never: the source's read-only guard admits only reads |
| `list_source_tables` | the catalog statements `connect::source_catalog_statements` names (the tables query, then the columns query when columns are asked for; `GET /v1/datasets` for lake's Polars service) | never |

Notes:

- **A statement the source's read-only guard refuses is refused, not warned.**
  It can never run, so offering to confirm it would be a lie.
  (`connector_query_rejection` decides.) In practice a connection warns only at
  `warn_all`.
- **An inline `connector`** (no saved connection) has no name to carry a level
  and runs at the connection default, under which reads never warn. Save the
  connection to set its level.
- `system.*` and the reserved stores are not user databases and never warn. A
  statement that names no database (`CREATE USER`, `GRANT`, `CREATE VIRTUAL
  TABLE`) is governed by its own rights.
- `create_dataset_from_query` also writes a table into a Zetta database (its
  `database` input). That write is not checked against that database's level;
  safe mode on a Zetta database covers the `sql` capability.

**The check order** in `Registry::call`: validate the input → authorize → safe
mode → consent → run. A caller that may not run a statement learns nothing
about it from a warning. A consent challenge freezes the input after the
confirmation was checked, so the approver approves a statement its requester
already confirmed.

## Reading and changing a level

| Capability | Kind | Input | Output |
|---|---|---|---|
| `get_safe_mode` | read (`GET /v1/get_safe_mode?database=sales`) | `{"database": "<name>"}` or `{"connection": "<name>"}` | `{"database"\|"connection": name, "mode", "is_default"}` |
| `set_safe_mode` | action (`POST /v1/set_safe_mode`) | the same selector plus `"mode": "off"\|"warn_writes"\|"warn_all"` | the same, plus `previous` |

`list_connections` and `create_connection` also return each connection's
`safe_mode`.

**Rights.** Both capabilities need the call right (`mcp:call`). Then:

| | Read the level | Change the level |
|---|---|---|
| a **database** | the call right | `sql:ddl` on that database (a flat grant or one `ON <database>.*`): the right to create and drop it |
| a **connection** | `list_connections` | `create_connection`: the right to save and replace it |

The selector must name exactly one thing that exists (`not_found` otherwise).
`system` and the reserved stores have no level.

**Audit.** Every change is a `safe_mode_changed` event: the actor, target
`database:<name>` or `connection:<name>`, detail `<previous> -> <new>`. Every
confirmed warning is a `safe_mode_confirmed` event: the actor, the same target,
and the capability, the level and the **statement hash** (the confirmation).
Neither carries a statement or a credential.

## Where the level lives

In the reserved database `zetta_safe_mode`, table `levels`, one row per
database: `{target, name, mode, updated, updated_by}`
(`include/zetta/governance/safe_mode.hpp`). It is an ordinary replicate-to-all
`data::Table`, the pattern the connection store uses: the row reaches every node
through CRDT gossip, and a standalone node keeps it on disk. SQL cannot reach
the database (the executor's reserved-store refusal), and `delete_dataset`
refuses it. Two nodes setting a level at the same instant can each add a row;
the latest `updated` wins on read (a tie goes to the stricter level), and the
next write replaces them all.

Dropping a database (`Node::drop_database`) or deleting a connection
(`delete_connection`) clears its level, so a new one under the same name starts
at the default.

## The pieces

| Piece | Where |
|---|---|
| Levels, defaults, store, confirmation, redaction | `include/zetta/governance/safe_mode.hpp`, `src/core/governance/safe_mode.cpp` |
| The check | `Registry::call` in `src/core/api/registry.cpp`; `Capability::describe_safe_mode` (`SafeModeDescriber`) in `include/zetta/api/registry.hpp`; `Registry::add` adds `safe_mode_confirmation` to each guarded input schema |
| What each capability runs | `describe_sql_safe_mode`, `describe_query_safe_mode`, `describe_catalog_safe_mode` in `src/core/api/capabilities.cpp`; `connect::source_catalog_statements` |
| `set_safe_mode` / `get_safe_mode` | `src/core/api/safe_mode_capabilities.cpp` |
| Audit actions | `safe_mode_changed`, `safe_mode_confirmed` (`governance/audit_event.hpp`) |
| Tests | `tests/api/test_safe_mode.cpp` |

## Not covered yet

- The desktop app's SQL console and the governed console run statements through
  `QueryService` directly, not the registry, so safe mode does not apply there.
- A pipeline step that reads a saved connection (the distributed ingest a `run`
  drives) is not checked; the capabilities above are.
- The web console renders a `safe_mode_warning` with its details but has no
  Run / Cancel dialog yet. Lake's dialog is tracked on
  [lake#906](https://github.com/oramasearch/lake/issues/906).
