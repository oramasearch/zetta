# End-to-end integration test (`tests/e2e/zetta-e2e.sh`)

The **e2e** is the outermost of Zetta's three testing levels. Unlike the others, it does
not build or link anything — it drives a **live, deployed node** over its HTTP API
(`:4800`) and asserts on real behavior.

| Level | What | Where it runs |
|---|---|---|
| **1 — unit tests** | the C++ `zetta-tests` binary (~1,242 cases) | build-time CI (`ctest`). **Not** run by the e2e. |
| **2 — the test suite** | the in-app [`zetta::testkit`](test-suite.md) catalog — the same cases the desktop **Operations → Test suite** panel runs | in-process (unit CI) **and**, for the e2e, driven over the `testsuite_run` capability against the live node |
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

The assertions are written against what the engine actually does (verified live against a local headless
node), captured rather than assumed:

- Every capability returns the envelope **`{ok, value, trace_id, span_id, duration_ms}`** (errors carry
  `{ok:false, error:{code,message}}`). SQL results live at **`.value.rows`** as **named objects**
  (`[{col: val, …}]`); the harness counts with `.value.rows | length` and reads fields by key (it also
  tolerates the older flat `.rows` shape so one script spans engine versions).
- Aggregates **name their output** (`sum(price)` → `sum_price`); **no bare `count(*)`** (aggregates
  require `GROUP BY`); **`GROUP BY` is numeric-only** (the DDF engine, zetta#756); **no `ORDER BY`**;
  **single-predicate `WHERE` only** (`AND`/`OR`/`NOT` are unsupported). These are asserted as tracked
  `SKIP`s. The store is **schemaless**, so a missing *column* resolves to null (not an error) — the
  negative-path assertion is a missing *table*, which is rejected.
- Run status is lowercase **`succeeded`** and `system.runs` keys on **`run_id`**.
- The in-app suite is submitted over **`testsuite_run`** (`testsuite_list` + `system.test_cases`); an
  older image that served `testkit_run` is auto-detected from the MCP tool list.
- `/mcp` is **JSON-RPC 2.0 over POST**; the harness sends an SSE `Accept` and reads a plain-JSON or
  `data:`-framed reply either way. `tools/call` returns `structuredContent {ok, value|error}` +
  `trace_id`/`span_id`/`duration_ms`; unknown tool → `unknown_capability`, bad args → `invalid_input`,
  unknown method → JSON-RPC `-32601`.
- Serving: `expose_index` takes **`{"index": id}`**, `expose_model` **`{"model": id}`**; the served
  search route is **`/v1/indexes/{name}/search?query=…`**. An **unknown `/v1/<name>` route blocks**
  (no default 404), so the harness only hits registered routes.
- `system.test_cases` and the user/role/token/policy authz DDL + the newer `system.*` views
  (`policies`/`roles`/`tokens`/`users`/`user_roles`/`test_cases`) are **not in every image** — L2 falls
  back to a built-in case list, and the missing views/DDL `SKIP` until that build is deployed.

### Configuration (environment)

| Variable | Default | Meaning |
|---|---|---|
| `ZETTA_E2E_BASE_URL` | *(required)* | the node's HTTP API, e.g. `http://<name>-zetta.<ns>.svc.cluster.local:4800` |
| `ZETTA_E2E_PEER_URLS` | — | space/comma list of the **other** nodes' base URLs (per-pod DNS); enables the replication canary when ≥2 nodes |
| `ZETTA_E2E_LEVELS` | `2,3` | which levels to run (`2`, `3`, or `2,3`) |
| `ZETTA_E2E_TOKEN` | — | bearer token for the target node — with auth always on (#799) every node requires one (the seeded/auto-generated admin token). **Required** for the §3.13 authz-enforcement assertions (`ZETTA_E2E_REQUIRE_AUTH=1`) |
| `ZETTA_E2E_REQUIRE_AUTH` | `0` | `1` enables the §3.13 authz-enforcement assertions; the target enforces auth **unconditionally** now (#799), so this only turns the extra assertions on |
| `ZETTA_E2E_DB` | `e2e` | scratch SQL database |
| `ZETTA_E2E_TIMEOUT` / `ZETTA_E2E_RETRIES` / `ZETTA_E2E_POLL` | `45` / `10` / `240` | per-request timeout, per-request retries, async-run poll budget (seconds) |

### Run it by hand

```sh
# against a local headless node
zetta --headless --mcp-listen 127.0.0.1:4800 --data-dir /tmp/z &
ZETTA_E2E_BASE_URL=http://127.0.0.1:4800 sh tests/e2e/zetta-e2e.sh

# level 3 only
ZETTA_E2E_BASE_URL=http://127.0.0.1:4800 ZETTA_E2E_LEVELS=3 sh tests/e2e/zetta-e2e.sh

# authz profile — §3.13 enforcement assertions (regression harness for #759-#769)
# auth is always on (#799); --admin-token seeds the owner so the harness has a bearer
zetta --headless --mcp-listen 127.0.0.1:4801 --data-dir /tmp/z-auth \
      --admin-token "$TOK" &
ZETTA_E2E_BASE_URL=http://127.0.0.1:4801 ZETTA_E2E_REQUIRE_AUTH=1 ZETTA_E2E_TOKEN="$TOK" \
  sh tests/e2e/zetta-e2e.sh

# multi-node — §3.10 CRDT + convergence (lights up once #786 lands)
ZETTA_E2E_BASE_URL=http://node-a:4800 ZETTA_E2E_PEER_URLS="node-b:4800 node-c:4800" \
  sh tests/e2e/zetta-e2e.sh
```

## Coverage

**Level 2** (#775) discovers the case list (from `system.test_cases`, falling back to a built-in
canonical list), submits each via `testsuite_run` (a *job* capability returning a durable `run_id`;
an older image serving `testkit_run` is auto-detected from the MCP tool list), and polls `system.runs`
(keyed on `run_id`) to a lowercase terminal status — `succeeded` is a pass,
`failed`/`error` a fail, a case the build omits a `SKIP`. It then asserts **catalog completeness**
(the core categories ran) and reports per-category counts. So the entire testkit catalog (compute /
DDF / ML / cluster / governance / UC-1 / example scripts) runs inside the deployed binary.

**Level 3** sections (each a labeled block in the script; this is the coverage map, and the epic #774
tracks growing it). The six epic children (#777–#782) are the sections marked with their issue:

| § | Area | Covered / issue |
|---|---|---|
| 3.1 | identity + membership | `/v1/server_info`, `system.nodes` count |
| 3.2 | SQL DDL/DML | `CREATE DATABASE`/`TABLE`, `INSERT`, `SELECT *` count, point lookup by key |
| 3.3 | query engine (#776) | `=`/`<>`/`<`/`>`/`<=`, bool+string filters, numeric `GROUP BY`+aggregate, missing-table rejection; compound `WHERE` + `ORDER BY` + string `GROUP BY` are tracked SKIPs |
| 3.4 | `system.*` shape + content (**#777**) | all ~29 views readable; `nodes` count == node count + membership cols; `tables` catalog names every view; `datasets` carries the e2e table; `audit`/`spans`/`traces`/`runs`/`test_cases` non-empty with asserted column shapes |
| 3.5 | MCP protocol (**#778**) | SSE-tolerant `/mcp`: `initialize`→`serverInfo`, `ping`, `tools/list` (all tools), `tools/call` over every read tool (`structuredContent {ok,value,trace_id,span_id,duration_ms}`), error paths (unknown tool → `unknown_capability`, bad args → `invalid_input`, unknown method → `-32601`) |
| 3.6 | REST surfaces (**#778**) | every `GET /v1/<read>`, `/v1/openapi.json`, `/v1/specs`(`/:name`), `/llms.txt`\|`-full`\|`/:topic` |
| 3.7 | orchestration | `POST /v1/create_project` (action) + read-back |
| 3.8 | serving (**#779**) | negatives always: search/predict on a nonexistent entity → clean 4xx (not a hang), `expose_index` bad args → 400; positive expose→search→`exposed` flag→unexpose→denied round-trip when the target carries an index (else a tracked SKIP — no black-box index/model-build surface) |
| 3.9 | governance views | `principals`/`keys`/`users`/`roles`/`user_roles`/`tokens`/`policies`/`audit` readable; `system.tokens` carries no secret material |
| 3.10 | cluster + CRDT (**#781**) | membership on every node; **bidirectional** A→B and B→A convergence (latency-bounded); rebalance + blob transfer. **Gated on ≥2 reachable nodes** (mTLS-sync #786): SKIPs cleanly on a single-node target, lights up on a real cluster — never a FAIL |
| 3.11 | observability (**#782**) | `system.activity`/`spans`/`traces` carry the e2e's ops; `get_trace` round-trip; `system.crashes` empty |
| 3.12 | blobs / consent / audit (**#782**) | missing-object → clean 4xx; consent-gated capability → `403 consent_required` (no side effect), listable + audited, local session cannot self-decide, bogus id → clean error; audit monotonic growth across the e2e's writes. Blob put/get + approve→execute are tracked SKIPs (no black-box surface / need an approver key) |
| 3.13 | authz enforcement (**#780**) | under the always-on auth gate (#799; regression harness for **#759–#769**): deny-by-default on REST + `/mcp`, bogus bearer denied, admin authenticates; reader denied write (#759) + role escalation (#761); RLS `USING (current_user())` + admin bypass (#748/#762); no-condition policy rejected (#765); `system.tokens` no secrets (#744). Privileged-DDL gap (#759), masking/scoped-grants (#747/#760/#767), token revoke, catalog exposure (#766) are tracked SKIPs that light up as each finding's enforcement lands |

Every SKIP names its reason and the issue it awaits, so a green run is a precise statement of what the
target can and cannot yet enforce. The **#780** privileged-statement checks are gated on the build's
base write-enforcement (a build where `sql:write` is unenforced can't give a trustworthy RLS/masking
verdict) — they assert strictly once enforcement is intact.

## How it is operated in production

The script lives here so it evolves with the engine, and is packaged into the
**`oramasearch/zetta-e2e`** runner image (Alpine + `curl` + `jq` + this script; built in
`zetta-bridge/build/images/zetta-e2e/`). The **orama-operator** owns a
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
