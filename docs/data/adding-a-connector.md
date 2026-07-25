# Adding a source connector — the practice

The repeatable method for teaching zetta to pull from a new source. It adapts the
Amaro-desktop connector practice (a Perplexity deep-dive per source → a
structured connector-skill doc → build) to zetta's native, read-only,
distributed-ingest connector framework. Two connectors exist as worked examples:
**Snowflake** (HTTP+SQL API) and **Postgres** (a wire-protocol driver).

The steps use two skills already in the repo — **`perplexity`** (generate the
research prompt) and **`new-connector`** (build the adapter) — plus the
connector-skill doc format below. Do them in order; do not skip the research.

## 1. Research — a Perplexity deep-dive (the connector skill)

Before any code, understand the source. Use the **`perplexity`** skill to
generate a deep-research prompt covering, for this source:

- **Reach the data:** the API/protocol. Is there an **HTTP + SQL/REST API**
  (→ native HTTP client, like Snowflake), or does it need a **wire protocol /
  vendor driver** (→ a driver, coroutine-wrapped, like Postgres/libpqxx)? URL(s),
  versioning.
- **Auth:** the credential kind(s) (bearer/PAT, OAuth, password, key-pair,
  service account), how it's presented, and what's the least-privilege read-only
  scope.
- **Read semantics:** how you run a **read-only** query or list objects;
  pagination; **rate limits**; result partitioning (does the result arrive in
  independently-fetchable chunks, like Snowflake's `partitionInfo`, so ingest can
  fan out?).
- **Key objects + fields:** the tables/objects worth pulling and their important
  columns.
- **Join keys:** how this source's records correlate to others (email → domain,
  account id, etc.) — the value for downstream pipelines.
- **Gotchas:** silent exclusions, type quirks, auth failure modes, anything
  "blocked behind customer login."

Record the answers as a **connector-skill doc** (the format in the appendix) at
`docs/data/connectors/<source>.md`. This is the durable artifact the build reads
from and the product surfaces later.

## 2. Decide the transport (the ADR-17 gate)

- **HTTP + SQL/REST API** → build native on the **injected `HttpTransport` seam**
  (no SDK) — the Snowflake path. This is the ADR-17-clean default.
- **Wire protocol only** (raw Postgres, MySQL, …) → a **vendor driver** is a
  **deliberate ADR-17 exception**, decided per source. When taken (as for
  Postgres/libpqxx): vendor it as a submodule, name it in **one adapter TU** in
  the `zetta-api` target (never `zetta-core`), inject it through a runner seam,
  and **coroutine-wrap every blocking call with `runtime::offload`** so zetta's
  threading is never disturbed. Record the exception in the source's doc + here.

## 3. Build — the `new-connector` skill

Follow the **`new-connector`** skill. In brief, mirroring the two examples:

1. **Config:** add the source's fields to `ConnectorConfig`
   (`include/zetta/connect/connector.hpp`); the single secret rides in `token`
   (redacted by `to_json`); validate per-type in `from_json`; add the fields to
   `connector_config_schema()` and `"<source>"` to `connector_types()`.
2. **Adapter TU:** `src/core/connect/<source>_protocol.cpp` (HTTP API) or a
   `zetta-api` TU behind a runner seam (driver). Map the source's columns to
   **typed JSON cells** (numbers stay numbers). Non-throwing — every failure is a
   `ConnectorResult::error` reason.
3. **Read-only guard:** reuse `read_only_query_rejection` (SELECT/WITH only,
   single statement); a driver adds a read-only transaction + never-commit + a
   row cap.
4. **Dispatch:** wire the type in `run_connector_query`.
5. **Ingest** rides the existing `create_dataset_from_query` path (rows → sharded
   + placed across the cluster). A partition-aware source can fan out later, like
   Snowflake.

## 4. Test + docs

- A **guarded** connector test against a real instance (skips when none is
  reachable / the build lacks the driver — the `test_postgres_connector.cpp`
  pattern), plus pure `from_json`/guard cases.
- Schema-as-code: regenerate `schemas/*.json` + `openapi.json`.
- Update `docs/data/connector-query.md`, this file's example list, and the
  source's connector-skill doc; wire the fields into the web `SourceConnections`
  panel + the desktop connection form.

## Appendix — the connector-skill doc format

One `docs/data/connectors/<source>.md` per source (adapted from Amaro's
`connector-skills/`):

```
# <Source> — connector skill
## Summary            one paragraph: what it is, when/why to pull it
## Transport + auth   API/protocol, credential kind, least-priv read scope
## Read semantics     how to read-only query, pagination, rate limits, partitioning
## Key objects        the tables/objects + important fields worth pulling
## Join keys          how it correlates to other sources (the stitch keys)
## Gotchas            exclusions, type quirks, auth failure modes
## Status             researched | building | shipped; the tracking issue
```
