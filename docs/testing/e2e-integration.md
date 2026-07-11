# End-to-end integration test (`tests/e2e/zetta-e2e.sh`)

The **e2e** is the outermost of Zetta's three testing levels. Unlike the others, it does
not build or link anything — it drives a **live, deployed node** over its HTTP API
(`:4800`) and asserts on real behavior.

| Level | What | Where it runs |
|---|---|---|
| **1 — unit tests** | the C++ `zetta-tests` binary (~1,242 cases) | build-time CI (`ctest`). **Not** run by the e2e. |
| **2 — the test suite** | the in-app [`zetta::testkit`](test-suite.md) catalog — the same cases the desktop **Operations → Test suite** panel runs | in-process (unit CI) **and**, for the e2e, driven over the `testkit_run` capability against the live node |
| **3 — complete integration** | black-box assertions over every deployed surface: SQL + the `system.*` views, MCP, REST, serving, governance, **CRDT replication across nodes**, cluster membership, audit | against the live node/cluster |

**The e2e runs level 2 and level 3.** Level 2 proves the engine's own catalog passes *inside the
deployed binary*; level 3 proves the surfaces that the in-process suite never touches — the real
`:4800` HTTP server, the `:7777` peer plane, and cross-node convergence.

## The runner — `tests/e2e/zetta-e2e.sh`

A POSIX-`sh` (busybox-`ash`-safe) script, `curl` + `jq` only. It retries every request hard
(the node HTTP server drops back-to-back requests — [#731](https://github.com/oramasearch/zetta/issues/731)),
is idempotent against a persistent PVC (per-run unique table/user/project names — the engine is
append-only, [#735](https://github.com/oramasearch/zetta/issues/735)), and adapts to the node
count (the replication canary skips on a single-node target).

Every check is **PASS / FAIL / SKIP**. `SKIP` is for a genuinely-unavailable surface (no GPU, the
OramaCore engine not compiled in, a single-node cluster, a capability this build omits) — it is
*tracked*, never a silent pass. Any `FAIL` exits non-zero. **Never weaken an assertion to make it
pass — extend the engine instead** (the same forcing-function rule as the rest of the suite).

### Configuration (environment)

| Variable | Default | Meaning |
|---|---|---|
| `ZETTA_E2E_BASE_URL` | *(required)* | the node's HTTP API, e.g. `http://<name>-zetta.<ns>.svc.cluster.local:4800` |
| `ZETTA_E2E_PEER_URLS` | — | space/comma list of the **other** nodes' base URLs (per-pod DNS); enables the replication canary when ≥2 nodes |
| `ZETTA_E2E_LEVELS` | `2,3` | which levels to run (`2`, `3`, or `2,3`) |
| `ZETTA_E2E_TOKEN` | — | optional bearer token (the deployed node is unauthenticated-local by default) |
| `ZETTA_E2E_DB` | `e2e` | scratch SQL database |
| `ZETTA_E2E_TIMEOUT` / `ZETTA_E2E_RETRIES` / `ZETTA_E2E_POLL` | `45` / `10` / `240` | per-request timeout, per-request retries, async-run poll budget (seconds) |

### Run it by hand

```sh
# against a local headless node
zetta --headless --mcp-listen 127.0.0.1:4800 --data-dir /tmp/z &
ZETTA_E2E_BASE_URL=http://127.0.0.1:4800 sh tests/e2e/zetta-e2e.sh

# level 3 only
ZETTA_E2E_BASE_URL=http://127.0.0.1:4800 ZETTA_E2E_LEVELS=3 sh tests/e2e/zetta-e2e.sh
```

## Coverage

**Level 2** enumerates `system.test_cases`, submits each via `testkit_run` (a *job* capability
returning a durable `run_id`), and polls `system.runs` to a terminal status — `Succeeded` (Pass or
Skipped) is a pass, `Failed` is a fail. So the entire testkit catalog (compute / DDF / ML /
cluster / governance / UC-1 / example scripts) runs inside the deployed binary.

**Level 3** sections (each is a labeled block in the script — this list is the coverage map, and it
is meant to grow):

1. **identity + membership** — `/v1/server_info`, `system.nodes`
2. **SQL DDL/DML** — `CREATE DATABASE`/`TABLE`, `INSERT`, `SELECT count`
3. **query engine** — `WHERE` filters, `GROUP BY`, `sum`/`max`
4. **`system.*` views** — the observability catalog is readable
5. **MCP protocol** — `initialize` → `serverInfo`, `tools/list`
6. **REST reads** — `GET /v1/list_*`, `/llms.txt`
7. **orchestration** — `create_project` + read-back
8. **serving** — `expose_index` + `/v1/indexes/:name/search`
9. **governance** — `CREATE USER`, `system.principals`, the audit trail *(deny-by-default
   enforcement engages only under `--require-auth`; the deployed node is unauthenticated-local, so
   this asserts existence, not rejection)*
10. **CRDT replication** — write on node A, converge on node B (needs ≥2 nodes)

## How it is operated in production

The script lives here so it evolves with the engine, and is packaged into the
**`oramasearch/zetta-e2e`** runner image (Alpine + `curl` + `jq` + this script; built in
`zetta-bridge/build/images/zetta-e2e/`). The **orama-cloud-v1-operator** owns a
`ZettaClusterE2ETest` CRD that runs the image as an in-cluster Job against a deployed
`OramaZettaCluster` and surfaces pass/fail in the CR status — the same pattern as its
`OramaCloudV1E2ETest`. This is part of **One Orama / One Amaro**
([orama-bridge#130](https://github.com/oramasearch/orama-bridge/issues/130)); the operating +
expansion workflow is codified in the `one-orama-dev` skill.

## Adding coverage

- **A new engine behavior** → add a `zetta::testkit` case (see [test-suite.md](test-suite.md)); the
  e2e picks it up automatically at level 2 (it enumerates `system.test_cases`).
- **A new deployed-surface assertion** → add a numbered block to the level-3 section of
  `zetta-e2e.sh` using the `sql` / `http` / `pass` / `fail` / `skip` helpers, and add its line to
  the coverage map above. Prefer a deterministic assertion (exact count/value); `skip` only when the
  surface is legitimately absent.
- Rebuild + push the `oramasearch/zetta-e2e` image so the operator picks up the change (the script
  travels in the image, not in the operator binary).

See also [mcp-journeys.md](mcp-journeys.md) (simulated-user journeys over the same surfaces) and
[test-suite.md](test-suite.md) (the level-2 catalog).
