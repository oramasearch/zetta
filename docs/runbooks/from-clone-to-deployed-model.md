# From a clean clone to a deployed model

The whole customer path in one procedure: **build → run a node → connect a coding agent
over MCP → train a model on your own data → ask the node how to deploy itself → package
that into a Helm chart.**

This is the runbook an enterprise evaluator follows on their own hardware, with no help
from us. That is the bar [#974](https://github.com/oramasearch/zetta/issues/974) set —
*"you bring your own Helm chart here, and then you tell me, and then I'll build with it"* —
and it is not specific to any one customer.

**Every command and number below was executed against a live node on `v0.1.0`**
(commit `b54c008`, the first tagged release), not transcribed from documentation. Where a
step has a trap, the trap is stated before the step.

Related: [`docs/build.md`](../build.md) (the full build reference + troubleshooting) ·
[`docs/api/registry-design.md`](../api/registry-design.md) (why REST and MCP cannot drift) ·
[`docs/ml/held-out-metrics.md`](../ml/held-out-metrics.md) (why §4.4 insists on the holdout) ·
[`docs/data/dataset-path.md`](../data/dataset-path.md) (the `dataset_path` grammar and the
column contract).

---

## 1 · Build

```sh
git clone --recurse-submodules git@github.com:oramasearch/zetta.git
cd zetta && git submodule update --init --recursive
```

`--recurse-submodules` is not optional, and the **`--recursive`** matters —
`automerge-cpp` nests its own submodules, and a non-recursive init fails at configure with
a missing `upstream/json`.

Three submodules are private (`corosio`, `oramacore-cpp`, and the product repo itself) and
are pinned by **SSH** URL, so an SSH key has to be loaded before the submodule step — an
HTTPS-only setup fails there with a permission error that reads like a missing grant.
Everything else resolves publicly.

### 1.1 · OpenSSL stops configure first — before any compiler question

The most common clean-host failure, on every platform:

```
CMake Error at CMakeLists.txt:202 (find_package):
  Could NOT find OpenSSL, try to set the path to OpenSSL root folder in the
  system variable OPENSSL_ROOT_DIR (missing: OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)
```

| Platform | Fix |
|---|---|
| Ubuntu | `sudo apt-get install -y libssl-dev` — the base image carries the *runtime* `libssl3`, not the headers |
| macOS (Apple Silicon) | `brew install openssl@3` |
| macOS (Intel) | `brew install openssl@3` **and** `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` |
| Non-standard prefix | `-DOPENSSL_ROOT_DIR=<prefix>` |

**It is `REQUIRED` by design and must not be made optional to simplify a build template.**
OpenSSL backs the cluster's inter-node **mTLS** (Corosio's TLS provider) and
**node-identity certificate minting** (`src/core/cluster/security.cpp`). It is the
dependency behind the `"peer_transport": "mTLS, always"` / `"plaintext_mode": false` facts
that `deployment_profile` reports in [§5](#5--ask-the-node-how-to-deploy-itself). A build
that quietly omitted it would be a node that talks plaintext to its peers without saying so.

> **Known gap.** The `if(APPLE)` branch of `CMakeLists.txt` hardcodes
> `/opt/homebrew/opt/openssl@3`, the **Apple-Silicon** Homebrew prefix. On an Intel Mac,
> `openssl@3` can be installed correctly and configure still fails. `brew --prefix
> openssl@3` would resolve both architectures.

### 1.2 · Configure and build

**macOS:**

```sh
brew install cmake ninja llvm openssl@3 ccache libpq   # or: ./scripts/install-deps.sh
cmake -S . -B build -G Ninja
cmake --build build
```

CMake auto-selects Homebrew LLVM; Apple Clang cannot do C++26 and the configure says so
rather than failing later.

**Ubuntu 24.04 / x86_64:**

```sh
sudo apt-get update && sudo apt-get install -y \
  libssl-dev ninja-build ccache lld zlib1g-dev pkg-config \
  libpq-dev libstdc++-14-dev gcc-14 g++-14
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && sudo /tmp/llvm.sh 22
sudo snap install cmake --classic

cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-22 -DCMAKE_C_COMPILER=clang-22 \
  -DSDL_UNIX_CONSOLE_BUILD=ON
cmake --build build -j "$(nproc)"
```

`-DSDL_UNIX_CONSOLE_BUILD=ON` is required on a headless host — SDL3 aborts configure
without X11/Wayland headers, and a server wants neither.

### 1.3 · The one that silently costs a feature

**`modules/libpqxx` must be initialized or the Postgres connector is not in the binary.**
CMake guards it on the submodule's presence and logs the decision:

```
-- Postgres source connector (libpqxx): ENABLED
```

If that reads `DISABLED`, the build is green and `create_connection` with
`"type": "postgres"` fails at *query* time instead of at build time. Check the configure
output before going further.

---

## 2 · Run a node

```sh
./build/bin/zetta --headless --data-dir ~/zetta-data
```

The owner token prints **once**, is hashed at rest, and is never shown again. For a
repeatable deploy, seed it with `--admin-token` / `ZETTA_ADMIN_TOKEN` instead.

```sh
export ZETTA=http://127.0.0.1:4800
export TOK=<the token it printed>
curl -s -H "Authorization: Bearer $TOK" $ZETTA/v1/server_info
```

**The two flags that get confused, which have cost more time than anything else here:**

| Flag | Binds |
|---|---|
| `--listen HOST:PORT` | the **cluster peer** port (default 7777) |
| `--mcp-listen HOST:PORT` | the **API, MCP and web UI** (default `127.0.0.1:4800`) |

Set `--listen`, then call the port you chose, and you get nothing — which reads exactly
like a dead node.

**Run headed on a workstation** if you want the UI in front of you — the desktop app is
the same node with a renderer. It is no longer *required* for anything in this runbook:
consent gating is off unless you pass `--governance`, so a headless node is a complete
node ([§4.2](#if-you-are-on-an-older-build-this-call-is-a-wall)).

| Flag | Effect |
|---|---|
| *(default)* | consent gating **off** — cloud-touching calls execute and audit as `consent_bypassed` |
| `--governance` | the first call to a cloud-touching capability returns a one-time consent challenge that a **different** principal must approve |

Three routes are public — readable without a token, deliberately, because you have to be
able to learn how to run a node before you hold one's credential:
`$ZETTA/cli.md` · `$ZETTA/mcp.md` · `$ZETTA/v1/openapi.json`.

---

## 3 · Connect a coding agent over MCP

```sh
claude mcp add --transport http zetta "$ZETTA/mcp" \
  --header "Authorization: Bearer $TOK"
```

**Measured on `v0.1.0`:** `tools/list` returns **75 tools** — the same capability registry
that renders the REST routes and the OpenAPI spec, so an agent and a curl see identical
facts by construction.

> **`prompts/list` advertises a `build_and_run` prompt, and it will fail.** It drives
> `agent_run`, which needs an installed agent, and a fresh node has none
> ([#977](https://github.com/oramasearch/zetta/issues/977)). Use the tools directly — the
> path in §4 needs no model at all.

---

## 4 · Train a model on your own data

Six calls, all on both surfaces.

### 4.1 · Which surface — and "REST is more deterministic" is false

**Measured, both surfaces, same node, same data:** the identical run over `POST /mcp`
`tools/call` and over `POST /v1/…` returned `held_out_auc: 0.7911784511784512` — the same
float to the last digit. MCP additionally returns a typed `structuredContent` block, and
surfaces failures as `isError: true` carrying the same structured error code that REST
returns. **The engine is exactly as deterministic over MCP.**

What differs is **who composes the calls**:

| | REST / curl | MCP / coding agent |
|---|---|---|
| Picks the arguments | you | the model |
| Reproducible on a second run | byte-for-byte | only if the model chooses the same way |
| Failure mode | a typo, which errors loudly | a *plausible* wrong argument — the wrong `label_column`, or skipping `approve_plan` and reading `not_approved` as a bug |
| Best at | anything that has to be repeated or quoted | exploring an unfamiliar schema, and §5 |

Use curl for a result you will repeat; use the agent to explore a schema
(*"look at `sales.accounts`, decide which column is the label, build a churn Pipeline and
run it with a 30% holdout"*) and **always** for [§5](#5--ask-the-node-how-to-deploy-itself).

> **Reads are `GET`, actions and jobs are `POST` — and getting it backwards looks exactly
> like a hung node.** `POST` a read and the reply comes back **empty**: no error, no body,
> curl eventually times out. It has already cost a filed issue
> ([#954](https://github.com/oramasearch/zetta/issues/954)) and it will cost you an hour if
> you do not know it. The rule is mechanical — if the capability only *reads*
> (`sql`, `score`, `explain`, `get_run`, `get_run_report`, `list_connections`,
> `list_consent_requests`, `server_info`), it is `GET` with the arguments in the query
> string:
>
> ```sh
> curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/sql" \
>   --data-urlencode 'statement=SELECT * FROM sales.accounts LIMIT 5;'
> ```
>
> Anything that changes state (`create_connection`, `create_dataset_from_query`, `run`,
> `approve_plan`, `gate`) is `POST` with a JSON body. `$ZETTA/v1/openapi.json` states the
> method for every route, and **MCP has no such distinction** — every tool is one
> `tools/call`, which is one reason the agent path is more forgiving here.

Every step below gives the curl. The MCP equivalent is the tool of the same name with the
same arguments object:

```sh
curl -s -X POST "$ZETTA/mcp" -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"compile_intent","arguments":{"project":"churn"}}}'
```

### 4.2 · Land the data

**A file — no network at all, and the path that works on any build:**

```sh
curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  $ZETTA/v1/add_dataset -d '{
    "path": "/tmp/accounts.csv", "database": "sales",
    "table_name": "accounts", "partition_only": true
  }'
```

`partition_only: true` is correct for anything connector-sized. Without it every row is
captured into the replicated document, and reconciling them back rewrites the whole local
store per row, saturating the node's owner loop
([#952](https://github.com/oramasearch/zetta/issues/952)). Parquet, JSONL and JSON behave
identically.

**A live connection — two calls, and the second one is the demo.**

```sh
curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  $ZETTA/v1/create_connection -d '{
    "name": "local-pg",
    "connector": {
      "type": "postgres", "host": "127.0.0.1", "port": "5432",
      "user": "readonly_user", "token": "<password>",
      "database": "sales", "sslmode": "disable"
    }
  }'
```

Discrete fields, **not a DSN**. Then verify the credential claim rather than asserting it:
`GET /v1/list_connections` returns the full connector config **with `token` absent**. The
secret is never serialized back out by any surface — worth having an evaluator check
themselves.

Now pull rows straight out of the database into a partitioned dataset — no export, no
file:

```sh
curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  $ZETTA/v1/create_dataset_from_query -d '{
    "connection": "local-pg",
    "query": "SELECT account_id, mrr, seats, failed_payments, churned FROM accounts",
    "database": "sales",
    "table_name": "accounts"
  }'
```

**Measured** against a local Postgres 17: `ok: true`, with the columns, the row count, and
one entry per partition placed on this node. Read it back with the same scatter-gather
`SELECT` any other dataset uses:

```sh
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/sql" \
  --data-urlencode 'statement=SELECT account_id, mrr FROM sales.accounts WHERE failed_payments > 3;'
```

Only a single `SELECT` / `WITH` reaches the source — the read-only guard rejects anything
else at the boundary, before a connection is opened. `max_rows` defaults to 100000; a
connector result set lands at replication factor 1 because it is re-pullable from its
source, and `promote_dataset` raises it if the dataset turns out to be worth keeping.

**Two measured traps:**

- **`token` is required even for a trust-auth Postgres.** An empty `token` is refused with
  `connection: the connector config carries no token`, so a laptop Postgres on local
  `trust` needs a password set on the role first.
- **The node's SQL is a Postgres *subset*.** `LIMIT` is not supported (`expected ';' after
  statement, got 'LIMIT'`) and a bare `count(*)` returns `aggregate functions require a
  GROUP BY clause`; projection and `WHERE` work. The `SELECT` inside
  `create_dataset_from_query` runs on **the source** and may use anything the source
  understands — only the read back out of the node goes through the subset.

**A remote source is slower than it looks and a local one is not distributed.** Tiling a
result set across the cluster needs a result *manifest* from the source, which Snowflake's
SQL API v2 provides and Postgres has no equivalent of. So a Postgres pull is one node
reading one stream, then partitioning what it got. It is correct and it is not the
aggregate-bandwidth path — do not present it as one.

#### Shape the source query for the model, not just for the read

Two column rules are enforced at the `connect` step — *after* the dataset lands — so a
query that ingests happily can still fail the pipeline later with a message that does not
point back here. Both are fixed in the **source** query.

| Rule | What you see if you miss it | Fix in the SELECT |
|---|---|---|
| The **entity column must be a 64-bit integer** | `run` refuses: ``dataset: the entity column 'account_id' holds non-integer values; entities are keyed by a 64-bit integer`` | derive one — `('x'\|\|substr(md5(account_id),1,8))::bit(32)::bigint AS account_key` |
| The **label and every feature must be numeric** | the run starts, then fails: ``connect step … row 0 field 'churned' is not a number`` | cast it — `churned::int AS churned` |

A Postgres `boolean` label and a text/UUID primary key are the two common ways to hit
these, and most real tables have both. String entity keys are a known limitation, not a
defect ([`../data/dataset-path.md`](../data/dataset-path.md) — *"String entity keys"*).

**Measured end to end on 600 rows**, the source query that satisfies both:

```sql
SELECT ('x'||substr(md5(account_id),1,8))::bit(32)::bigint AS account_key,
       mrr::float8 AS mrr, seats, failed_payments,
       churned::int AS churned
FROM accounts
```

§4.3's `run` then takes `"entity_column": "account_key"`.

#### If you are on an older build, this call is a wall

Worth knowing, because it looks like a product failure and it is what an evaluator on a
pre-fix binary will hit. Two separate bugs sat on top of `create_dataset_from_query`:

**The consent gate refused it.** It was gated per *tool*, so a query against `127.0.0.1`
came back as *"touches a cloud provider"* — and the refusal named the web console's
`/governance` page, which has no consent list, while the desktop app's approval surface is
on **Admin & audit**. A single-operator workstation also has no second principal, so the
challenge could be issued and then decided by nobody.

**And underneath it, the call had never worked on Postgres at all.** Every connector
config went through Snowflake's REST protocol regardless of `type`, so a Postgres
connection built a Snowflake request out of its empty `account` and failed with
`snowflake: could not resolve .snowflakecomputing.com` — a vendor nobody named. The gate
had been hiding it.

Both are fixed
([#994](https://github.com/oramasearch/zetta/issues/994),
[#995](https://github.com/oramasearch/zetta/issues/995)). On a current build:

- **Consent enforcement is off by default.** A consent-gated capability executes on the
  first call, and the ungated call is recorded as a `consent_bypassed` audit event naming
  what ran. Start the node with `--governance` (or `ZETTA_GOVERNANCE=1`) to require
  approval again.
- **A loopback source is not a cloud.** Even with `--governance` on, a `postgres`
  connector on `127.0.0.1` / `localhost` / `::1` or a unix socket is not gated — nothing
  leaves the machine.
- **A remote source's challenge names the real connector**, e.g.
  `postgres db.example.com:5432/sales`, and lists your own query as what would run.

If you see the `consent_required` refusal or the `.snowflakecomputing.com` error against a
local database, you are on a build from before those fixes. `add_dataset` above is the
path that works on any build.

### 4.3 · Project → intent → Pipeline → Verification → run

```sh
P() { curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
      "$ZETTA/v1/$1" -d "$2"; }

P create_project  '{"name":"churn","description":"Which accounts are about to leave, and why"}'
P capture_intent  '{"project":"churn","text":"predict which accounts are likely to churn, with calibrated scores and explanations"}'
P compile_intent  '{"project":"churn"}'
```

**Measured** — `compile_intent` returns a 7-step Pipeline, 6 edges:

```
connect  Load customer accounts
connect  Load usage & billing activity
clean    Build churn features
model    Train churn-risk model
index    Index churn scores
serve    Serve churn scores & reasons
act      Trigger retention outreach
```

**A freshly compiled Pipeline is `proposed`, and Verification refuses to run on it.** The
error is `not_approved`, which does not read like an approval-flow problem, and it is the
step most often missed:

```sh
P approve_plan '{"project":"churn","graph":"churn skeleton"}'
P gate         '{"project":"churn","graph":"churn skeleton"}'
```

**Measured** — verdict `pass`, `block_deploy: false`, four checks. **Read the reasons, not
the statuses.** On a skeleton with no declared features, `data_leakage` passes because
there was nothing to check — *"no ML step declares features to check for leakage"*. That is
an honest pass, not a clean bill of health, and the report says which it is.

```sh
P run '{"project":"churn","graph":"churn skeleton",
        "dataset_path":"sales.accounts",
        "dataset":{"entity_column":"account_key","label_column":"churned"},
        "holdout_fraction":0.3}'
```

`account_key`, not the table's own text id — a text entity column is refused here and a
boolean label fails inside the run. See
[§4.2](#shape-the-source-query-for-the-model-not-just-for-the-read).

**Set `holdout_fraction`.** Without it every metric is in-sample — scored on the rows the
model trained on. This is not hypothetical: an in-sample AUC of 0.868 once reached a
client-facing report as the headline number when the held-out figure for the same run was
0.726 ([#953](https://github.com/oramasearch/zetta/issues/953)).

The call returns a `run_id` immediately; poll `get_run` until `succeeded`.

### 4.4 · Read what it found

```sh
curl -s -H "Authorization: Bearer $TOK" "$ZETTA/v1/get_run_report?run_id=<id>&limit=20"
```

**Measured on a 600-account, 38.3%-churn dataset, holdout 0.3:**

| | |
|---|---|
| held-out AUC | **0.791** (174 rows) |
| in-sample AUC | 0.940 (426 rows) |
| held-out accuracy | 0.707 |
| majority-class accuracy | **0.617** — what a constant answer scores |
| top drivers | `failed_payments` 105.2 · `seats` 39.6 · `total_events` 29.1 · `mrr` 26.2 |

**Quote the held-out figure, never the in-sample one.** The report prints both and labels
which is which; the *gap* between them is the overfitting signal and is only readable when
both are present.

**The report volunteers its own weaknesses.** On the built-in 24-row demo dataset the same
call returns a `caveats` block, verbatim:

> *"measured on only 4 held-out rows. Below about 30 a ranking metric is noise … Do not
> quote it as a result"* · *"a perfect score. On real data this is almost always a
> trivially separable demo set or a leaked feature, not a model that solved the problem."*

On the 600-row run, `caveats` was empty. Showing a skeptical audience that the system
flags its own bad numbers is worth more than the numbers.

### 4.5 · Interrogate the model

```sh
# the ranked at-risk list, after the run that produced it has ended
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/score" \
  --data-urlencode "model=<model-id>" --data-urlencode "dataset_path=sales.accounts"

# why THIS account — including one that is not in the data at all
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/explain" \
  --data-urlencode "model=<model-id>" \
  --data-urlencode 'row={"failed_payments":3,"total_events":9,"mrr":537,"seats":10,"support_severity":2,"tenure_days":400}'
```

**Measured** — `explain` returns a signed contribution per feature, strongest first
(`failed_payments` +2.82, `total_events` +1.22, the rest 0.0), a `base_value`, and the
resulting `risk` 0.983.

Two parameter names that trip people: `explain` takes **`row`** (singular) where `score`
takes **`rows`**; and `explain` with `entity=` requires `dataset_path` alongside it, or it
returns ``entity` needs `dataset_path` to read it from``.

**The distinction worth making to an evaluator:** `score`'s drivers are the *population*
ranking and are identical for every row. `explain` is the only thing that answers why
*this* account is at risk — two accounts with the same score can be at risk for opposite
reasons, and only `explain` shows it
([`docs/ml/explaining-one-row.md`](../ml/explaining-one-row.md)).

---

## 5 · Ask the node how to deploy itself

```sh
curl -s -H "Authorization: Bearer $TOK" "$ZETTA/v1/deployment_profile?target=helm"
```

A **read** capability, so it is on the MCP tool surface and the agent allowlist — the
intended use is a coding agent connected to the node being told *"produce a Helm values
file for this,"* and calling this instead of guessing. Every figure is computed from the
running binary, so it cannot drift from a document.

**Measured output, `target=helm`, verbatim:**

```json
{
  "helm": {
    "shape": "StatefulSet + a headless Service on the peer port + a client Service on the API port",
    "probes": { "readiness": "GET /v1/server_info on the API port",
                "note": "the API answering is the readiness signal; the peer port is not" },
    "values": {
      "image":       { "repository": "<your registry>/zetta", "tag": "<version>" },
      "service":     { "api": { "port": 4800 },
                       "peer": { "port": 7777, "clusterIP": "None" } },
      "persistence": { "enabled": true, "mountPath": "/var/lib/zetta" },
      "env":         { "ZETTA_ADMIN_TOKEN": "<from a Secret — never inline>",
                       "ZETTA_CLUSTER_TOKEN": "<from a Secret>",
                       "ZETTA_SEED_PEER": "<headless-service>:7777" },
      "args":        ["--headless", "--data-dir", "/var/lib/zetta"]
    }
  },
  "requires": {
    "runtime": ["one statically linked binary", "a data directory"],
    "none_of": ["external Postgres", "Redis", "Python at runtime",
                "Node.js at runtime", "a package manager at runtime"]
  },
  "security": {
    "peer_transport": "mTLS, always", "plaintext_mode": false,
    "external_egress_required": false,
    "egress_note": "the web UI serves no external assets — no CDN, no external fonts — so an air-gapped node works"
  }
}
```

The three fields that carry a platform review:

- **`none_of`** — enterprise build templates commonly stock Redis, Postgres, Go and Python.
  A node uses none of them at runtime.
- **`plaintext_mode: false`** — there is no plaintext mode to disable or justify.
- **`external_egress_required: false`** — this is about the **runtime**. The build is a
  different matter; see [§6.2](#62--the-build-time-egress-list).

`?target=generic` drops the `helm` block and keeps the ports, storage, peering and security
posture.

---

## 5b · Whether to ship the operator too

`orama-cloud-v1-operator`'s `OramaZettaCluster` CRD creates **exactly** what
`deployment_profile` describes by hand: a StatefulSet of equal peers, a headless Service
for peer DNS on 7777, a client Service on 4800, a per-node PVC at `/data`, and two Secrets
(the cluster join token and the seeded admin token).

**For a single cluster in an assigned slot, a plain chart does all of that**, and the
operator adds no capability the evaluator needs on day one. What it *does* add is
provisioning — nginx Ingress, Cloudflare DNS records, Karpenter NodePools, AWS IAM/IRSA —
which is precisely the surface a locked-down enterprise platform reserves for itself. It
also needs a **ClusterRole** (namespaces, nodes, daemonsets, HPAs) and **installs its own
CRDs**, which is the single hardest thing to get through a security review.

So the default answer is **one image, not two**:

- **Phase 1** — one image, one chart, one StatefulSet. Zetta is its own datastore, so there
  is nothing else to provision, and it runs inside an assigned slot without asking the
  cluster for anything.
- **Phase 2** — the operator, once there is a reason: several Zetta slices across k8s
  clusters joined into one CRDT cluster, elastic capacity, or the operator using Zetta as
  its own datastore. It ships its own Helm chart, so it packages the same way then.

Phase 1 is also the honest form of a **no-privilege deployment mode** — it proves the claim
rather than promising it.

---

## 6 · Package it

### 6.1 · The image

The container build lives in **`zetta-bridge`**, not in this repo:
[`build/images/zetta-headless/Dockerfile`](https://github.com/oramasearch/zetta-bridge/tree/main/build/images/zetta-headless).

```sh
# from the zetta-bridge checkout; the build context is the zetta source tree
docker buildx build --platform linux/amd64 \
  -f build/images/zetta-headless/Dockerfile \
  -t <registry>/zetta:<version> --push modules/zetta
```

Runtime stage installs **`libssl3`, `libstdc++6`, `libpq5`, `ca-certificates`** and nothing
else. Non-root (`uid 10001`), `VOLUME /data`, `EXPOSE 7777 4800`, and a default command
that binds both ports on `0.0.0.0`.

**Be precise about "no runtime dependencies."** There is no runtime *service* to provision —
no database, cache, interpreter, or package manager. That is the claim that survives a
reviewer. It is not a fully static binary, and saying so invites a check that fails.

The container build uses **Clang 19** (`apt.llvm.org`) and **CMake 3.31.5** (Kitware release
tarball) — a different pair from the bare-metal Ubuntu path above, and the friendlier one:
no snap, and an older Clang is likelier to already exist in a customer's template library.

### 6.2 · The build-time egress list

The runtime needs no egress. **The build does**, and this is the complete list — four
pinned CMake `FetchContent` fetches plus the toolchain:

| What | Pinned version | Source |
|---|---|---|
| SDL | `release-3.2.0` | `github.com/libsdl-org/SDL` |
| Dear ImGui | `v1.91.5` | `github.com/ocornut/imgui` |
| Boost | `1.91.0-1` (cmake tarball) | `github.com/boostorg/boost/releases` |
| json-schema-validator | `2.4.0` | `github.com/pboettch/json-schema-validator` |
| Clang toolchain | 19 (container) / 22 (bare metal) | `apt.llvm.org` |
| CMake | 3.31.5 (container) | `github.com/Kitware/CMake/releases` |
| OS packages | `libssl-dev`, `libpq-dev`, … | distro `apt` |

All four FetchContent sources are **public and version-pinned**, so all four mirror into an
internal artifact store. GoogleTest joins the list when tests are on; the container build
sets `-DZETTA_BUILD_TESTS=OFF`. SDL and ImGui are **build-time only** — the binary links
them and headless never initializes either.

This table is the answer to *"what does your build need at the OS prompt"*, which is the
first question any locked-down build-template system asks.

---

## 7 · What this release does not include

State these before an evaluator finds them.

- **No conversational agent.** No checkpoint ships, and a fresh node has zero agents
  ([#977](https://github.com/oramasearch/zetta/issues/977)) — Studio's Chat screen and the
  Ask Orama rail have nothing behind them. Everything in §4 is the deterministic path and
  needs no model, which is also why it is the reliable one. Open defects confined to the
  conversational surface: [#975](https://github.com/oramasearch/zetta/issues/975)
  (`POST /v1/chat` hangs over REST while MCP is fine),
  [#976](https://github.com/oramasearch/zetta/issues/976) (the agent names a tool instead
  of calling it), [#979](https://github.com/oramasearch/zetta/issues/979).
- **Discover Opportunities is a dead end in the UI** — the card offers something you cannot
  begin, because the Workspace Profile screen does not exist. Tracked, not papered over.
- **Postgres and Snowflake are the shipped connectors.** Anything else is a connector to
  write. Only Snowflake ingests with the cluster's aggregate bandwidth — a Postgres pull is
  one node reading one stream and then partitioning it, because Postgres returns no result
  manifest to tile ([§4.2](#42--land-the-data)). Snowflake's connector is also **PAT/OAuth
  only**: an account that authenticates with an RSA key pair cannot be reached yet.
- **Consent gating is off by default**, and turning it on with `--governance` leaves you
  with no reachable approval surface on a single-operator node — the web console has no
  consent list and the desktop app's is on Admin & audit
  ([#995](https://github.com/oramasearch/zetta/issues/995)). Approving over the wire needs
  a second principal holding `orchestration:approve_consent`.
- **One methodology is executable.** The compiler produces the tabular-supervised shape
  (M-1) plus part of the NLP/embeddings shape; the other nineteen in the catalogue are
  specified and not built ([#984](https://github.com/oramasearch/zetta/issues/984)). Churn
  is M-1, so this path is inside the built surface — a forecasting or uplift ask is not,
  and the failure there is *silent* (a plausible M-1 skeleton with the nouns swapped)
  rather than loud.

---

## Related

- [`docs/build.md`](../build.md) — the full build reference and troubleshooting.
- [`regenerating-the-run-report.md`](regenerating-the-run-report.md) — reproduce every
  figure in the churn reports from a clean checkout.
- The **zetta-walkthrough** skill — walks this same path adversarially and records what
  lied. This runbook is the happy path; that skill exists to find where it is not.
