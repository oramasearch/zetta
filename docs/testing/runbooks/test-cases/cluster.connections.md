# Saved connections: cluster-wide, read-only, no leak · `cluster.connections`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_connect.cpp` · **Engine doc:** [connector-query.md](../../../data/connector-query.md)

The connector **connection surface** over a real multi-node cluster, driven by an
**injected fake transport** — no network, no credentials, no warehouse anywhere in
the process. It pins the four properties the credential path stands on: a saved
connection reaches every node, a write statement never gets as far as a request,
the token appears on no read surface, and a query comes back **typed**
([#827](https://github.com/oramasearch/zetta/issues/827)).

## What it does

1. **Brings up an N-node replicated loopback cluster** (`LoopbackOptions{.replicated
   = true}`) and announces every node as a replicated member.
2. **Saves one connection on node 0** with a per-run unique token, then polls
   patiently (a seconds-long deadline, ticking each node) until **every** node
   resolves it through [`find_connection`](../../../data/connector-query.md) *with
   the credential intact* — the resolution path is the only caller allowed to see
   it, and every node needs it because every node fetches its own stripe. It also
   confirms the store is an ordinary **replicate-to-all** table, which is *why* the
   row lands everywhere rather than through a bespoke fan-out.
3. **Runs the read-only guard** against three write statements — `DROP TABLE`,
   `INSERT INTO`, and a write smuggled behind a read (`SELECT 1; DROP TABLE …`) —
   each through a transport that **counts its calls**. First it checks the guard
   still accepts a plain `SELECT`, so "refuses everything" cannot pass.
4. **Sweeps every read surface on every node** for the token:
   `SavedConnection::to_redacted_json()`, `ConnectorConfig::to_json()`, the
   `system.connections` SQL view, and the `list_connections` capability. It also
   asserts the *one* serialization that must carry it — the store's own
   `to_storage_json()` — still does.
5. **Runs a read-only query** through a canned Snowflake SQL API v2 response
   carrying a text, a number, a boolean and two nulls.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test suite
→ Distributed & cluster → Saved connections: cluster-wide, read-only, no leak**. Set
the options in the modal and click **Run**; the case runs on a worker thread and
stands up its own cluster, so the UI stays responsive. The result pane shows
pass/fail, the metrics, and the log.

**Headless:** the case is exercised (at `nodes=2`) by
`tests/testkit/test_testkit.cpp::ConnectorCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.ConnectorCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 2–6 | cluster size: how many nodes must end up holding the connection |

## What it asserts (pass criteria)

- **It round-trips cluster-wide.** Every node resolves the connection by name and
  reads back the **exact token** that was saved — on a patient poll, not a fixed
  count. The backing table's replication strategy is the default
  (**ReplicateToAll**) and it has no partitioner.
- **The read-only guard fires before egress.** All three write statements are
  rejected and **the transport is never called** (`0` requests) — the refusal
  happens while building the request, not after it. A plain `SELECT` is still
  accepted.
- **The credential is on no read surface.** On *every* node the token is absent
  from `to_redacted_json()`, `ConnectorConfig::to_json()`, the `system.connections`
  row, and the `list_connections` capability output — while
  `to_storage_json()` still carries it (the one place it lives).
- **Cells come back typed.** `text` → string, `fixed` → number (`1200`, not
  `"1200"`), `boolean` → bool, `NULL` → JSON null — so a landed dataset is
  featurizable rather than a table of strings.

## Metrics reported

`nodes` · `resolved on` (N/N nodes) · `write statements refused` · `transport calls
during refusal` · `read surfaces checked` · `query requests` · `typed columns` ·
`rows`.

## Related

- Engine: [connector-query.md](../../../data/connector-query.md) ·
  [replication-strategy.md](../../../data/replication-strategy.md) ·
  [system-datasets.md](../../../data/system-datasets.md)
- Sibling cases: [`cluster.distributed_import`](cluster.distributed_import.md) —
  the ingest this connection feeds ·
  [`governance.audit`](governance.audit.md)
