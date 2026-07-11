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

Every check is **PASS / FAIL / SKIP**, with a precise rule:

- **FAIL** — the surface is present but returned the **wrong answer**. Exits non-zero.
- **SKIP** — the surface is **genuinely absent** in this build: the engine *rejected* the statement
  (a documented limit), a capability/view is missing, the cluster is single-node, or auth is off. It
  is *tracked*, never a silent pass.

This "engine-rejects → SKIP, wrong-answer → FAIL" split (via the `assert_count` / `assert_ok` /
`assert_reject` helpers) is what lets one script run green across engine versions without masking a
real regression. **Never weaken an assertion to make it pass — extend the engine instead.**

### Deployed-engine behaviors the harness is grounded on

The assertions are written against what the **deployed** engine actually does (verified live), not the
HEAD source — they differ:

- SQL rows come back as **named objects** (`rows: [{col: val, …}]`), not positional arrays — the
  harness counts with `.rows | length` and reads fields by key.
- **No bare `count(*)`** (aggregates require `GROUP BY`); **`GROUP BY` is numeric-only** (the DDF
  engine, zetta#756); **no `ORDER BY`**. These are asserted as tracked `SKIP`s.
- Run status is lowercase **`succeeded`** and `system.runs` keys on **`run_id`**.
- `system.test_cases` and the user/role/token authz DDL are **not in every image** — L2 falls back to a
  built-in case list, and the authz DDL/enforcement checks `SKIP` until that build is deployed.

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

**Level 2** (#775) discovers the case list (from `system.test_cases`, falling back to a built-in
canonical list), submits each via `testkit_run` (a *job* capability returning a durable `run_id`),
and polls `system.runs` (keyed on `run_id`) to a lowercase terminal status — `succeeded` is a pass,
`failed`/`error` a fail, a case the build omits a `SKIP`. It then asserts **catalog completeness**
(the core categories ran) and reports per-category counts. So the entire testkit catalog (compute /
DDF / ML / cluster / governance / UC-1 / example scripts) runs inside the deployed binary.

**Level 3** sections (each a labeled block in the script; this is the coverage map, and the epic #774
tracks growing it):

| § | Area | Covered / issue |
|---|---|---|
| 3.1 | identity + membership | `/v1/server_info`, `system.nodes` count |
| 3.2 | SQL DDL/DML | `CREATE DATABASE`/`TABLE`, `INSERT`, `SELECT *` count, point lookup by key |
| 3.3 | query engine (#776) | `=`/`<>`/`<`/`>`/`<=`, `AND`/`OR`/`NOT`, bool+string filters, numeric `GROUP BY`+aggregate, unknown-column rejection; `ORDER BY` + string `GROUP BY` are tracked SKIPs |
| 3.4 | `system.*` views (#777) | all 23 views readable |
| 3.5 | MCP protocol (#778) | `initialize`→`serverInfo`, `tools/list`, `tools/call server_info` |
| 3.6 | REST reads (#778) | every `GET /v1/<read>`, `/v1/openapi.json`, `/llms.txt` |
| 3.7 | orchestration | `create_project` + read-back |
| 3.8 | serving (#779) | `expose_index` + `/v1/indexes/:name/search` |
| 3.9 | governance | `system.principals`/`keys`/`audit` readable; authz DDL SKIP until deployed |
| 3.10 | CRDT replication (#781) | write on node A → converge on node B (needs ≥2 nodes) |
| 3.11 | observability (#782) | `system.runs`/`spans` carry the e2e's ops; `system.crashes` empty |
| 3.12 | authz enforcement (#780) | under `--require-auth`: unauthenticated request denied; full matrix pending |

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
