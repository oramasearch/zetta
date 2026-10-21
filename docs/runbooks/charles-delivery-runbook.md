# Orama Studio (Zetta) — delivery runbook for Charles

**Release:** `v0.1.0` · commit `b54c008` on `main`, tagged **2026-08-12** — the first tagged
release of the engine · **For:** Charles Philippe (Motive)

**What this runbook covers, end to end:** the GitHub access he needs → clone and build →
run a node → point a coding agent at it over MCP → **build a churn model on his own
Postgres data** (driven either from the agent or from curl — both measured, both identical)
→ ask the node how to deploy itself → package that into Motive's golden Helm chart.

**One image, not two.** The Kubernetes operator is **not** part of this delivery — see
[§5b](#5b--do-we-ship-the-kubernetes-operator-too--no-and-that-is-good-news). This
corrects the two-component answer given on the Aug 5 call, and it makes the ask smaller.

**Every command and every number below was executed against a live node on 2026-08-12**,
not read out of documentation. Where a step has a trap, the trap is stated before the
step, not after it.

> ⚠️ **`v0.1.0` cannot do §4.1's connector path.** Two bugs — a consent gate with no
> reachable approver, and a Postgres query dispatched through Snowflake's protocol — made
> `create_dataset_from_query` unusable against a local database. Both are fixed on
> [PR #996](https://github.com/oramasearch/zetta/pull/996) and **neither fix is in the
> tagged `v0.1.0`**. Decide before the session which build he gets, and read
> [§4.2](#42--two-bugs-that-used-to-stop-41-dead--and-which-build-has-them) first. On the
> tagged release, use the file path in [§4.1](#41--get-the-data-into-the-node) and do not
> demo the connector.

**The delivery contract:** Charles runs every step. Nobody at Orama touches Motive data,
sees a credential, or needs access to his machine.

> **Scope note.** This release delivers the **churn model path**. The conversational
> agent ("Ask Orama") is **not part of this delivery** — no checkpoint ships, and the
> steps below deliberately use the deterministic capability path instead. See
> [§7](#7--what-is-not-in-this-delivery).

> ⚠️ **This is the Motive-specific runbook, and it names Motive people and internals.**
> It records Charles Philippe by name, **Pat** and **Tony** by first name, Motive's build
> template, the technologies their templates carry, **the surface they block**, their
> `motive.*` schema, and their internal org timing around Aug 19. Charles has read access
> to `oramasearch/zetta`, so this is readable by the customer it concerns — which is the
> point — but **anyone else with read access to this repo also sees it.**
>
> [`from-clone-to-deployed-model.md`](from-clone-to-deployed-model.md) is the same technical
> path with **no customer content**. Use that one for any other evaluator.
>
> **Commercial terms are not in this file and must not be added.** Pricing, the gate table,
> and anything Charles said in confidence stay in
> [the Motive project folder](https://github.com/oramasearch/orama-bridge/tree/main/docs/projects/motive-fde-engagement)
> only.
>
> **When the technical path changes, change it in
> [`from-clone-to-deployed-model.md`](from-clone-to-deployed-model.md) first**, then carry
> the Motive specifics here.

Related: [`build-dependencies.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/projects/motive-fde-engagement/build-dependencies.md) (the measured dependency list
Charles asked for on Aug 5) · [`deploy-runbook.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/projects/motive-fde-engagement/deploy-runbook.md) (the working-session
narrative) · [`../../meetings/2026-08-05-casey-charles-steve/deployment-requirements.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/meetings/2026-08-05-casey-charles-steve/deployment-requirements.md)
(Motive's five-step deployment path, in his words).

---

## 0 · GitHub access — exactly four repos

The build needs the product repo, three of its vendored submodules, and the image-build
repo. **Three of the four are private**; everything else the build touches is public and
needs no grant.

| Repo | Why | Visibility |
|---|---|---|
| `oramasearch/zetta` | the product | **private** |
| `oramasearch/corosio` | vendored — coroutine networking (`modules/corosio`, branch `orama-main`) | **private** |
| `oramasearch/oramacore-cpp` | vendored — BM25F full-text (`modules/oramacore-cpp`) | **private** |
| `oramasearch/zetta-bridge` | the container image build (`build/images/zetta-headless/Dockerfile`) | **private** |

Grant read-only, per repo:

```sh
for r in zetta corosio oramacore-cpp zetta-bridge; do
  gh api -X PUT "repos/oramasearch/$r/collaborators/<charles-github-handle>" -f permission=pull
done
```

**Read-only (`pull`) is sufficient** for everything in this runbook, including the image
build.

**No other grant is needed.** The remaining submodules resolve publicly — `cppalliance/capy`,
`cppalliance/beast2`, `jtv/libpqxx`, `oramasearch/http`, `homer6/automerge-cpp` (and its
nested `homer6/automerge` + `nlohmann/json`). Verified 2026-08-12.

**One wrinkle worth telling him up front:** `modules/corosio`, `modules/oramacore-cpp` and
`modules/http` are pinned by **SSH** URL, so his GitHub SSH key has to be loaded before
`--recurse-submodules` — an HTTPS-only setup fails on the submodule step with a
permission error that reads like a missing grant.

---

## 1 · Clone and build

```sh
git clone --recurse-submodules git@github.com:oramasearch/zetta.git
cd zetta && git checkout v0.1.0 && git submodule update --init --recursive
```

`--recurse-submodules` is not optional, and the **`--recursive`** matters — `automerge-cpp`
nests its own submodules, and a shallow init fails at configure with a missing
`upstream/json`.

### macOS (his 32 GB M-series — the machine his Postgres is on)

```sh
brew install cmake ninja llvm openssl@3 ccache libpq   # or: ./scripts/install-deps.sh
cmake -S . -B build -G Ninja
cmake --build build
```

CMake auto-selects Homebrew LLVM; Apple Clang is too old for C++26 and the configure says
so plainly rather than failing later.

**`openssl@3` is required, not optional** — see the box below. On an **Intel** Mac,
Homebrew's prefix is `/usr/local`, which the build does not look in, so add
`-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`.

### Ubuntu 24.04 / x86_64 (what a Motive build template would be)

The exact package versions, measured on a clean provision, are in
[`build-dependencies.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/projects/motive-fde-engagement/build-dependencies.md). The short form:

```sh
sudo apt-get update && sudo apt-get install -y \
  libssl-dev \
  `# libssl-dev is NOT optional — configure fails before anything else without it` \
  ninja-build ccache lld zlib1g-dev pkg-config \
  libpq-dev libstdc++-14-dev gcc-14 g++-14
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && sudo /tmp/llvm.sh 22
sudo snap install cmake --classic

cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-22 -DCMAKE_C_COMPILER=clang-22 \
  -DSDL_UNIX_CONSOLE_BUILD=ON
cmake --build build -j "$(nproc)"
```

### The one that stops configure dead: OpenSSL 3

**This is the first thing that fails on a clean host, on either platform.** The build does
`find_package(OpenSSL REQUIRED)` and stops:

```
CMake Error at CMakeLists.txt:202 (find_package):
  Could NOT find OpenSSL, try to set the path to OpenSSL root folder in the
  system variable OPENSSL_ROOT_DIR (missing: OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)
```

| Platform | Fix |
|---|---|
| Ubuntu | `sudo apt-get install -y libssl-dev` — the base image has the *runtime* `libssl3`, not the headers |
| macOS (Apple Silicon) | `brew install openssl@3` |
| macOS (Intel) | `brew install openssl@3` **and** `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` — the build only looks in `/opt/homebrew` |
| Anywhere non-standard | `-DOPENSSL_ROOT_DIR=<prefix>` |

**It is required by design and must not be dropped to simplify a build template.** OpenSSL
backs the cluster's **inter-node mTLS** and **node-identity certificate minting** — it is
the dependency behind the `"peer_transport": "mTLS, always"` / `"plaintext_mode": false`
claim in [§5](#5--ask-the-node-how-to-deploy-itself). There is no build that quietly omits
it, which is the point: a reviewer cannot be handed a binary that silently talks plaintext.

### The one that silently costs a feature

**`modules/libpqxx` must be initialized or the Postgres connector is not in the binary.**
The CMake guards it on the submodule's presence and logs its decision:

```
-- Postgres source connector (libpqxx): ENABLED
```

If that line says `DISABLED`, the build is green and `create_connection` with
`"type": "postgres"` will fail at query time instead of at build time. Check the configure
output for it before moving on.

---

## 2 · Run a node

```sh
./build/bin/zetta --headless --data-dir ~/zetta-data
```

It prints the owner token **once**:

```
zetta: no --admin-token supplied — generated the owner 'owner' access token (id …).
SAVE THIS — it is shown only once:
    <64 hex characters>
```

It is hashed at rest and never shown again. For a repeatable deploy, seed it instead with
`--admin-token` / `ZETTA_ADMIN_TOKEN`.

```sh
export ZETTA=http://127.0.0.1:4800
export TOK=<the token it printed>
curl -s -H "Authorization: Bearer $TOK" $ZETTA/v1/server_info
```

**The two flags that get confused — this is the single most expensive misreading on the
project:**

| Flag | Binds |
|---|---|
| `--listen HOST:PORT` | the **cluster peer** port (default 7777) |
| `--mcp-listen HOST:PORT` | the **API, MCP and web UI** (default `127.0.0.1:4800`) |

Set `--listen`, then call the port you chose, and you get nothing — which reads exactly
like a dead node.

**Run it headed on his laptop** if you want the UI in front of him — the desktop app is
the same node with a renderer, and it demos better. It is no longer *required* for
anything here: consent gating is off unless the node is started with `--governance`, so
headless is a complete node
([§4.2](#42--two-bugs-that-used-to-stop-41-dead--and-which-build-has-them)).

| Flag | Effect |
|---|---|
| *(default)* | consent gating **off** — cloud-touching calls execute and audit as `consent_bypassed` |
| `--governance` | the first call to a cloud-touching capability returns a one-time consent challenge a **different** principal must approve |

Do not turn `--governance` on for his session; nothing in it needs the gate. If it is on,
a challenge is decided in the desktop app's **Governance** page (the person at the machine
may approve) or in the web console by a second user holding `cloud:manage`
([docs/api/consent.md](../api/consent.md#a-one-person-node)). Remote sources are guarded
per database by **safe mode** instead ([docs/api/safe-mode.md](../api/safe-mode.md)).

The node documents itself, and all three routes are public (no token needed to read them):
`$ZETTA/cli.md` (every flag) · `$ZETTA/mcp.md` (how to connect an MCP client) ·
`$ZETTA/v1/openapi.json` (the whole REST surface).

---

## 3 · Point a coding agent at it over MCP

The node speaks MCP over streamable HTTP at `POST $ZETTA/mcp`.

```sh
claude mcp add --transport http zetta "$ZETTA/mcp" \
  --header "Authorization: Bearer $TOK"
```

Claude Desktop / claude.ai: Settings → Connectors → **Add custom connector**, URL
`$ZETTA/mcp`, request header `Authorization: Bearer <token>`.

**Measured:** `tools/list` returns **76 tools** — the same capability registry
that renders the REST routes and the OpenAPI spec, so an agent and a curl see identical
facts. That set includes `deployment_profile`, `run`, `get_run_report`, `score` and
`explain` — every step below is reachable from the agent without Charles writing curl.

---

## 4 · Build the churn model

Six calls. **Every one is on both surfaces**, and they are the same calls either way — one
capability registry renders the REST route and the MCP tool from a single entry, so the
two cannot drift.

### Which surface — and the answer is not "REST is more deterministic"

**Measured, both surfaces, same node, same data:** the identical run over
`POST /mcp` `tools/call` and over `POST /v1/…` returned
`held_out_auc: 0.7911784511784512` — the same float, to the last digit. MCP also returns a
typed `structuredContent` block alongside the text, and surfaces failures as
`isError: true` carrying the same structured error code (`consent_required`, etc.) that
REST returns. **The engine is exactly as deterministic over MCP.**

What differs is **who composes the calls**:

| | REST / curl | MCP / coding agent |
|---|---|---|
| Who picks the arguments | Charles | the model |
| Reproducible on the second run | byte-for-byte | only if the model makes the same choices |
| Failure mode | a typo, which errors loudly | a *plausible* wrong argument — the wrong `label_column`, or skipping `approve_plan` and reading `not_approved` as a bug |
| Best at | the demo path, anything he'll repeat in front of Pat | exploring, reading the OpenAPI, and §5 |

**So use both, deliberately:**

- **Drive §4 with the curl calls below** when the run has to be reproducible — a demo, or
  a number that ends up on a slide.
- **Drive it from Claude Code over MCP** when he is exploring his own schema: *"look at
  `motive.accounts`, decide which column is the label, build a churn Pipeline and run it
  with a 30% holdout."* That is the product's actual pitch, it works today, and it is a
  much better first experience than pasting curl.
- **Always use the agent for [§5](#5--ask-the-node-how-to-deploy-itself)** — the Helm
  step was designed for exactly that and is the strongest thing to show him.

**Two prompts, and only one of them works.** `prompts/list` shows both as slash commands
in his client:

| Prompt | Use it? |
|---|---|
| **`model_from_source`** | **Yes — this is the one.** The whole deterministic path: browse the source, pull a model-ready frame, compile → approve → gate → run with a holdout, read the report, then `score` / `explain`. Needs no agent and no checkpoint. Takes `connection` (required), plus optional `ask` and `database`. |
| `build_and_run` | **No.** It drives `agent_run`, which needs an installed agent, and **no agent ships** ([§7](#7--what-is-not-in-this-delivery)). It will fail. |

`model_from_source` carries the three column rules from
[§4.1](#shape-the-source-query-for-the-model-not-just-for-the-read) inline, so an agent
following it applies the casts *before* the run rather than discovering them from a
failure. **Measured: an MCP client driving that prompt completed all 14 steps** —
connection → catalog → frame → project → compile → approve → gate → run → report → score →
explain — with no human correction.

Each step below gives the curl. To run the same step over MCP, call the tool of the same
name with the same arguments object:

```sh
curl -s -X POST "$ZETTA/mcp" -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"compile_intent","arguments":{"project":"churn","name":"churn skeleton"}}}'
```

### 4.0 · The one that will cost him an hour: reads are GET, actions are POST

**`POST` a read and the reply comes back empty** — no error, no body, curl hangs until it
times out. It looks exactly like a dead node, and it has already cost a filed issue
([zetta#954](https://github.com/oramasearch/zetta/issues/954)).

The rule is mechanical. If the capability only *reads* — `sql`, `score`, `explain`,
`get_run`, `get_run_report`, `list_connections`, `list_consent_requests`, `server_info` —
it is **`GET`**, arguments in the query string:

```sh
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/sql" \
  --data-urlencode 'statement=SELECT * FROM motive.accounts LIMIT 5;'
```

Anything that changes state — `create_connection`, `create_dataset_from_query`, `run`,
`approve_plan`, `gate`, `add_dataset` — is **`POST`** with a JSON body. `$ZETTA/v1/openapi.json`
gives the method for every route.

That is why some commands below use `-G` and some do not; it is not stylistic. **MCP has no
such distinction** — every tool is one `tools/call` — which is one reason the agent path in
§3 is more forgiving, and worth saying out loud if he asks why you keep switching.

### 4.1 · Get the data into the node

**Safest path — a file.** No network, no connector, and it works on every build:

```sh
psql -d motive -c "\copy (SELECT …) TO '/tmp/accounts.csv' CSV HEADER"

curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  $ZETTA/v1/add_dataset -d '{
    "path": "/tmp/accounts.csv",
    "database": "motive",
    "table_name": "accounts",
    "partition_only": true
  }'
```

`partition_only: true` is the right setting for anything connector-sized: without it every
row is replicated into the CRDT document and reconciling them back rewrites the local
store per row, which saturates the node's owner loop (that was [zetta#952](https://github.com/oramasearch/zetta/issues/952)).

Parquet, JSONL and JSON work identically.

**Live path — a saved connection.** This is the demo-worthy one, and it is where the
credential claim gets tested:

```sh
curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  $ZETTA/v1/create_connection -d '{
    "name": "local-pg",
    "connector": {
      "type": "postgres",
      "host": "127.0.0.1", "port": "5432",
      "user": "readonly_user", "token": "<password>",
      "database": "motive", "sslmode": "disable"
    }
  }'
```

Note the shape: `type` / `host` / `user` / `token` / `database` — **not** a DSN string.

Then verify the claim rather than believing it:

```sh
curl -s -H "Authorization: Bearer $TOK" $ZETTA/v1/list_connections
```

**Measured:** the response carries the full connector config **with `token` absent**. The
secret is stored and never serialized back out by any surface. Worth having him check
himself — it is exactly the kind of claim a security reviewer will ask about.

**Then look inside the source before pulling anything.** `list_source_tables` opens one
connection, runs one read-only catalog query, and **lands nothing** — this is how you find
a table rather than ingesting one to discover it exists:

```sh
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/list_source_tables" \
  --data-urlencode "connection=local-pg" --data-urlencode "match=account"
```

It returns each table with its columns and types. `match` is a case-insensitive substring —
`match=score` is how you locate scoring tables in a database you have not seen. Drop
`match` for everything, add `schema=public` to narrow, `include_columns=false` for just
names.

**Read the types it returns before writing the next query** — they tell you which columns
need casting, which is exactly what the next section is about.

**Then the call that is actually the demo** — rows out of his Postgres into a partitioned
dataset, no export step:

```sh
curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  $ZETTA/v1/create_dataset_from_query -d '{
    "connection": "local-pg",
    "query": "SELECT account_id, mrr, seats, failed_payments, churned FROM accounts",
    "database": "motive",
    "table_name": "accounts"
  }'
```

**Measured** against a local Postgres 17: `ok: true`, with the column list, the row count,
and one entry per partition placed on this node. Read it back the same way as any other
dataset — note the `-G`, `sql` is a read:

```sh
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/sql" \
  --data-urlencode 'statement=SELECT account_id, mrr FROM motive.accounts WHERE failed_payments > 3;'
```

Only a single `SELECT` / `WITH` reaches Postgres; the read-only guard rejects anything else
before a connection is opened, which is the other claim a security reviewer will poke at.

**Two traps in that read-back, both measured:**

- **`token` is required, even for a trust-auth Postgres.** `create_connection` with an
  empty `token` is refused with `connection: the connector config carries no token`. His
  `motive-pgvector` container sets `POSTGRES_PASSWORD=motive`, so he has one — but a
  laptop Postgres on local `trust` auth cannot be connected until a password is set on the
  role.
- **The node's SQL is a Postgres *subset*, and it is not the query you just sent to
  Postgres.** At the time of this delivery `LIMIT` was not supported (`expected ';' after
  statement, got 'LIMIT'`) and a bare `count(*)` was refused with `aggregate functions require
  a GROUP BY clause`; both work now (`ORDER BY`/`LIMIT`/`OFFSET`, and whole-table aggregates
  since zetta#1218 — see [the query layer](../data/query.md)). Projection and `WHERE` work. The full `SELECT` inside `create_dataset_from_query` runs on
  **Postgres** and can use anything Postgres understands; only the read *back out of the
  node* goes through the subset. Worth saying explicitly, because hitting the subset right
  after a rich source query reads as the product being broken.

**One thing not to oversell:** this is *not* the aggregate-bandwidth path. Tiling a result
set across the cluster needs a result manifest from the source — Snowflake's SQL API v2
has one, Postgres has no equivalent — so a Postgres pull is one node reading one stream and
then partitioning what it got. Correct, and single-node.

#### Shape the source query for the model, not just for the read

The dataset contract validates column roles before the Run starts. Ordinary warehouse
text identifiers and boolean targets need no source-query workaround.

| Rule | What you see if you miss it | Fix in the SELECT |
|---|---|---|
| The **entity column must be an integer or non-empty text value** | mixed or empty values are refused before the Run starts | keep the warehouse business id; every output restores it |
| The **label must be numeric/boolean 0/1; features numeric/boolean** | contract resolution names a non-numeric column and its cast | cast only other representations |

A Postgres `boolean` loads as 0/1. A text/UUID/Salesforce account id receives a stable,
collision-checked internal key and remains unchanged in outputs
([`docs/data/dataset-path.md`](../data/dataset-path.md)).

**The query that satisfies both, measured end to end on 600 rows:**

```sh
curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  $ZETTA/v1/create_dataset_from_query -d '{
    "connection": "local-pg",
    "query": "SELECT ('"'"'x'"'"'||substr(md5(account_id),1,8))::bit(32)::bigint AS account_key, mrr::float8 AS mrr, seats, failed_payments, churned::int AS churned FROM accounts",
    "database": "motive",
    "table_name": "accounts_ml"
  }'
```

Then §4.3's `run` takes `"entity_column": "account_key"`. **Keep the original
`account_id`** in a second dataset if he wants to join the ranked list back to readable
names — the model does not need it, and carrying a text column into the frame is what
causes the failure above.

**Do this before the session, on his data.** Every one of these three is a two-minute fix
once you know it and a dead session if you meet it live.

### 4.2 · Two bugs that used to stop §4.1 dead — and which build has them

**This is the section to check the build against.** Both were real, both are fixed, and a
binary from before the fix fails §4.1's connector path in two different ways that each
look like a product failure.

**The consent gate refused the call.** `create_dataset_from_query` was gated per *tool*, so
a query against a Postgres on `127.0.0.1` came back as "touches a cloud provider", pointing
at an approval page that does not exist:

```
consent_required — 'create_dataset_from_query' touches a cloud provider and did NOT
execute; consent challenge <id> was issued and is waiting for approval at /governance
(Consent requests). It must be approved by a DIFFERENT principal than the one that
requested it … approving from the same session that made the request is refused by design.
```

At the time the web console's `/governance` page had no consent list, the desktop app's
approval surface was on **Admin & audit** (not Governance), and a one-person laptop had no
second principal — so the challenge could be issued and then decided by nobody. Both
surfaces were fixed in [#999](https://github.com/oramasearch/zetta/issues/999).

**Underneath it, the call had never worked on Postgres at all.** Every connector config
went through Snowflake's REST protocol regardless of `type`, so his `postgres` connection
built a Snowflake request out of its empty `account`:

```
snowflake: could not resolve .snowflakecomputing.com
```

A vendor he never mentioned, for a database on his own machine. The gate had been hiding
it — nobody had ever got far enough to see it.

**On a build with the fixes:**

1. **Consent enforcement is off by default.** A node started without `--governance`
   executes consent-gated capabilities on the first call. The call is not unrecorded — it
   lands on the audit trail as `consent_bypassed`, naming exactly what ran. Start the node
   with `--governance` (or `ZETTA_GOVERNANCE=1`) to require approval again.
2. **A loopback source is not a cloud.** Even with `--governance` on, a `postgres`
   connector whose host is `127.0.0.1` / `localhost` / `::1` or a unix socket is not gated
   — nothing leaves the machine.
3. **A remote source's challenge names the real connector**, e.g.
   `postgres db.example.com:5432/motive`, and lists the caller's own query.
4. **Postgres ingests through its own wire protocol**, landing the same dataset shape the
   Snowflake path produces.

> ⚠️ **`v0.1.0` (commit `b54c008`) does NOT contain these fixes.** They are on
> [PR #996](https://github.com/oramasearch/zetta/pull/996)
> ([#994](https://github.com/oramasearch/zetta/issues/994),
> [#995](https://github.com/oramasearch/zetta/issues/995)). Until that merges and a tag
> carries it, **either give Charles a build from a branch that includes it, or route him
> down §4.1's file path and do not demo the connector.** Handing him `v0.1.0` and this
> section together is the one combination that guarantees a bad session: the runbook will
> say it works and the binary will say `.snowflakecomputing.com`.
>
> **Check before the session:**
>
> ```sh
> ./build/bin/zetta --help | grep -q -- --governance && echo "fixes present" || echo "PRE-FIX BUILD"
> ```

### 4.3 · Project → intent → Pipeline → Verification → run

```sh
P() { curl -s -X POST -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
      "$ZETTA/v1/$1" -d "$2"; }

P create_project '{"name":"churn","description":"Which accounts are about to leave, and why"}'

P capture_intent '{"project":"churn",
  "text":"predict which accounts are likely to churn, with calibrated scores and explanations"}'

P compile_intent '{"project":"churn","name":"churn skeleton"}'
```

`compile_intent` returns the Pipeline it built. **Measured — a 7-step graph, 6 edges:**

```
connect  Load customer accounts
connect  Load usage & billing activity
clean    Build churn features
model    Train churn-risk model
index    Score and index customer accounts
serve    Serve churn scores & reasons
act      Trigger retention outreach
```

**A newly compiled Pipeline is `proposed`, and Verification refuses to run on it** — a
step it is easy to miss, because the error is `not_approved` rather than anything about
approval flow:

```sh
P approve_plan '{"project":"churn","graph":"churn skeleton"}'
P gate         '{"project":"churn","graph":"churn skeleton"}'
```

**Measured — verdict `pass`, `block_deploy: false`, six checks:** `non_empty_graph`,
`schema_type`, `data_leakage`, `language_grounding`, `language_training_access`, and
`entity_resolution`. Read the *reasons*, not just the
statuses: on a skeleton with no declared features, `data_leakage` passes because there was
nothing to check — *"no ML step declares features to check for leakage"*. That is an
honest pass, not a clean bill of health, and the report says so.

Then run it against his data:

```sh
P run '{"project":"churn","graph":"churn skeleton",
        "dataset_path":"motive.accounts_ml",
        "dataset":{"entity_column":"account_key","label_column":"churned"},
        "holdout_fraction":0.3}'
```

The source `account_id` and boolean label are valid directly; see
[§4.1](#shape-the-source-query-for-the-model-not-just-for-the-read).

**Set `holdout_fraction`.** Without it every metric in the report is in-sample — scored on
the rows the model trained on — and they flatter. This is not theoretical: an in-sample AUC
of 0.868 once reached a client-facing report as the headline number when the held-out
figure for the same run was 0.726.

The call returns a `run_id` immediately; poll `get_run` until `succeeded`.

### 4.4 · Read what it found

```sh
curl -s -H "Authorization: Bearer $TOK" "$ZETTA/v1/get_run_report?run_id=<id>&limit=20"
```

**Measured on a 600-account / 38.3%-churn dataset, holdout 0.3:**

| | |
|---|---|
| held-out AUC | **0.791** (174 rows) |
| in-sample AUC | 0.940 (426 rows) |
| held-out accuracy | 0.707 |
| majority-class accuracy | **0.617** — the baseline a constant answer scores |
| top drivers | `failed_payments` 105.2 · `seats` 39.6 · `total_events` 29.1 · `mrr` 26.2 |

**Quote 0.791, never 0.940.** The report prints both and labels which is which; the *gap*
between them is the overfitting signal and is only readable when both are present.

**The report tells you when not to trust it.** On the built-in 24-row demo dataset the same
call returns a `caveats` block, verbatim:

> *"measured on only 4 held-out rows. Below about 30 a ranking metric is noise … Do not
> quote it as a result"* and *"a perfect score. On real data this is almost always a
> trivially separable demo set or a leaked feature, not a model that solved the problem."*

On the 600-row run, `caveats` was empty. That is the behaviour to show a skeptical
audience — it volunteers its own weaknesses.

### 4.5 · Interrogate it

Population ranking, and the per-account "why":

```sh
# the ranked at-risk list, after the run has ended
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/score" \
  --data-urlencode "model=<model-id>" --data-urlencode "dataset_path=motive.accounts"

# why THIS account — including one that is not in the data at all
curl -s -G -H "Authorization: Bearer $TOK" "$ZETTA/v1/explain" \
  --data-urlencode "model=<model-id>" \
  --data-urlencode 'row={"failed_payments":3,"total_events":9,"mrr":537,"seats":10,"support_severity":2,"tenure_days":400}'
```

**Measured** — `explain` returns a signed contribution per feature, strongest first
(`failed_payments` +2.82, `total_events` +1.22, everything else 0.0), a `base_value`, and
the resulting `risk` 0.983.

Two parameter names that will trip him: `explain` takes **`row`** (singular) while `score`
takes **`rows`**; and `explain` with `entity=` requires `dataset_path` alongside it, or it
returns ``entity` needs `dataset_path` to read it from``.

**The distinction worth making on the call:** `score`'s drivers are the *population*
ranking and are identical for every row. `explain` is the only thing that answers why
*this* account is at risk — two accounts with the same score can be at risk for opposite
reasons, and only `explain` shows it.

---

## 5 · Ask the node how to deploy itself

This is the capability built directly from Charles's own ask on the Aug 5 call —
*"given a Helm chart dependency, this is how you should configure this deployment
instance… at that point I don't need anything from you"* ([zetta#974](https://github.com/oramasearch/zetta/issues/974)).

```sh
curl -s -H "Authorization: Bearer $TOK" "$ZETTA/v1/deployment_profile?target=helm"
```

It is a **read** capability, so it is on the MCP tool surface and the agent allowlist —
the intended use is that he points a coding agent at the node and says *"produce a Helm
values file for this,"* and the agent calls this rather than guessing. Every figure is
computed from the running binary, so it cannot drift from a document.

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
    "peer_transport": "mTLS, always",
    "plaintext_mode": false,
    "external_egress_required": false,
    "egress_note": "the web UI serves no external assets — no CDN, no external fonts — so an air-gapped node works"
  }
}
```

**The three lines that matter for Motive's harness:**

- **`none_of`** — Motive's templates carry Redis, Postgres, Go and Python. Zetta uses
  **none** of them at runtime. That is the whole conversation with Tony, in one field.
- **`plaintext_mode: false`** — inter-node traffic is always mTLS with a shared join
  token. There is no plaintext mode to disable or justify to a reviewer.
- **`external_egress_required: false`** — this is about the **runtime**. The build is a
  different story; see [§6.2](#62--the-build-time-egress-list--the-real-blocker).

The generic form (`?target=generic`, or no parameter) drops the `helm` block and keeps
everything else — the ports, storage, peering and security posture.

---

## 5b · Do we ship the Kubernetes operator too? — **No, and that is good news**

On the Aug 5 call the component answer was **two images: the Orama operator (Go) and
Zetta (C++)**. Having gone through what the operator actually does for a Zetta cluster,
**this delivery needs one image, not two** — and Charles should be told that explicitly,
because he is about to socialize the component list with Tony before **Aug 19**. A
one-component ask is materially easier to get through a template review than a two-component
one, so correcting this makes the ask smaller, not larger.

**What `OramaZettaCluster` actually creates** is precisely what `deployment_profile`
already told us to write by hand:

- a **StatefulSet** of equal-peer nodes, each with its own PVC at `/data`,
- a **headless Service** for peer DNS on `7777`,
- a **client Service** for the API on `4800`,
- two Secrets — the 64-hex cluster join token (`ZETTA_CLUSTER_TOKEN`) and the seeded admin
  token (`ZETTA_ADMIN_TOKEN`).

For **one cluster in an assigned slot**, a plain chart does all of that. The operator adds
no capability Charles needs on day one.

**And everything else it adds is the exact surface Motive blocks.** The operator's other
job is provisioning: an **nginx Ingress**, **Cloudflare DNS records**, **Karpenter
NodePools**, and **AWS IAM/IRSA** roles. Compare that against what Charles said on the
call — *"this mapping of DNS records… they assign them"* and *"a lot of these things you're
showing here assumes a hands-on development environment access or clustered access for an
admin. But from an application deployment perspective, that's blocked, intentionally."*
Shipping the operator into Motive means shipping the one component whose main function
their platform forbids.

**It also costs the security review the most.** The operator needs a **ClusterRole** —
namespaces, nodes, daemonsets, HPAs — and it **installs its own CRDs**. That is a
cluster-scoped privilege grant, argued for in a golden-chart environment, in exchange for
nothing Charles can use yet.

**Say it as a phased answer, not as a retraction:**

> **Phase 1 (this delivery):** one image, one chart, one StatefulSet. Zetta is the whole
> platform — it is also its own datastore, so there is no Postgres or Redis to provision.
> Motive assigns the slot; Zetta runs in it and asks for nothing from the cluster.
>
> **Phase 2 (when it earns its keep):** the operator, once there is a reason — several
> Zetta slices across clusters joined into one CRDT cluster, elastic capacity, or the
> operator using Zetta as its own datastore. It already has its own Helm chart
> (`orama-operator/chart/`), so it packages the same way when the time comes.

This is also the honest form of the **no-privilege deployment mode** we already owe him
([§8](#8--checklist-for-today)): phase 1 *is* that mode, and shipping it first proves the
claim rather than promising it.

---

## 6 · Package it into the golden chart

Motive's path, [as Charles described it](https://github.com/oramasearch/orama-bridge/blob/main/docs/meetings/2026-08-05-casey-charles-steve/deployment-requirements.md):
point the compliance harness at the repo → let the analyzer rewrite the project → pass the
security tests → repackage into the golden chart → they assign placement.

### 6.1 · The image

The container build is in **`zetta-bridge`**, not in `zetta`:
`build/images/zetta-headless/Dockerfile`. Two stages, `ubuntu:24.04` both sides.

```sh
# from the zetta-bridge checkout; the build context is the zetta source tree
docker buildx build --platform linux/amd64 \
  -f build/images/zetta-headless/Dockerfile \
  -t <registry>/zetta:v0.1.0 --push modules/zetta
```

Runtime stage: **`libssl3`, `libstdc++6`, `libpq5`, `ca-certificates`** — that is the
entire runtime dependency set. Non-root (`uid 10001`), `VOLUME /data`, `EXPOSE 7777 4800`,
and a default command that already binds both ports on `0.0.0.0`.

Note the version skew worth flagging to Tony: the **container** build uses **Clang 19**
(from `apt.llvm.org`) and **CMake 3.31.5** (from the Kitware GitHub release tarball), while
[`build-dependencies.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/projects/motive-fde-engagement/build-dependencies.md) measured a bare-metal Ubuntu build on
Clang 22 + CMake 4.4.2 via snap. **The container numbers are the ones that matter for
Motive**, and they are the friendlier pair: no snap is involved, and Clang 19 is a far
likelier template entry than 22. Re-verify the image builds green at `v0.1.0` before
handing the number to IT.

### 6.2 · The build-time egress list — the real blocker

Charles's hard constraint: *"if it's external, we cannot download it at the time of build."*
The **runtime** needs no egress. The **build** does, and this is the complete list — four
pinned fetches that CMake performs at configure time, plus the OS packages:

| What | Pinned version | Source |
|---|---|---|
| SDL | `release-3.2.0` | `github.com/libsdl-org/SDL` |
| Dear ImGui | `v1.91.5` | `github.com/ocornut/imgui` |
| Boost | `1.91.0-1` (cmake tarball) | `github.com/boostorg/boost/releases` |
| json-schema-validator | `2.4.0` | `github.com/pboettch/json-schema-validator` |
| Clang toolchain | 19 (container) / 22 (bare metal) | `apt.llvm.org` |
| CMake | 3.31.5 (container) | `github.com/Kitware/CMake/releases` |
| OS packages | see [`build-dependencies.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/projects/motive-fde-engagement/build-dependencies.md) | Ubuntu `apt` |

All four FetchContent sources are **public and version-pinned**, so all four are
mirrorable into an internal artifact store. **This list is the deliverable for Tony** —
it is the answer to *"whatever is needed at the operating system prompt, we need to know
what that is."* SDL and ImGui are needed at **build** time only: the shipped binary links
them, and headless never initializes either.

### 6.3 · The dry run

Charles's own proposal, and the reason the deployment work is not blocked on SOC 2:

> *"Without deploying it, we will understand what's required from a Motive application
> standpoint to deploy it… That will give you time to get the SOC 2 Type 2 — but the
> minute you get it, we already will have the harness ready to deploy the next day."*

So the sequence for today is: build locally → run the churn model on his Postgres →
`deployment_profile?target=helm` → hand the image + the egress list to the harness **without
deploying** → collect what the analyzer demands.

Two things the analyzer will ask for that we should answer before it does:

- **"You connected to a Postgres — we'll create one."** Zetta *is* the datastore; it needs
  no external Postgres. The Postgres connection in §4 is a **source** he reads from, not a
  dependency the platform requires. `none_of` says this in the node's own words.
- **"You seeded data — ship a bootstrap that reseeds it."** There is no seeded state. A
  node boots empty with a data directory; `ZETTA_ADMIN_TOKEN` seeds the owner
  deterministically so an automated deploy never has to capture a printed token.

---

## 7 · What is **not** in this delivery

Say these plainly rather than letting him find them.

- **No conversational agent.** No model checkpoint ships with this release, and a fresh
  node has zero agents ([zetta#977](https://github.com/oramasearch/zetta/issues/977)).
  Studio's Chat screen and the "Ask Orama" rail have nothing behind them. Everything in
  §4 is the deterministic capability path and needs no model — which is also why it is
  the reliable path. Related open defects, all on the conversational surface only:
  [#975](https://github.com/oramasearch/zetta/issues/975) (`POST /v1/chat` hangs over REST
  while MCP is fine), [#976](https://github.com/oramasearch/zetta/issues/976) (the agent
  names a tool instead of calling it), [#979](https://github.com/oramasearch/zetta/issues/979).
- **Auto-Suggest / "Discover Opportunities" is a dead end in the UI.** The card explains
  something you cannot begin — the Workspace Profile screen does not exist. It is tracked,
  not papered over, but do not walk into it during a demo.
- **Postgres and Snowflake are the shipped connectors.** Anything else is a connector to
  write, not a project.
- **`create_dataset_from_query` on a local Postgres is fixed, but not in `v0.1.0`** —
  [§4.2](#42--two-bugs-that-used-to-stop-41-dead--and-which-build-has-them). Check the
  build before demoing the connector path.
- **Only Snowflake ingests with the cluster's aggregate bandwidth.** A Postgres pull is
  one node reading one stream and then partitioning it — Postgres returns no result
  manifest to tile. Correct, single-node, and not what "compute-near-data" describes.
- **Snowflake's connector is PAT/OAuth only.** Motive's `SVC_RETAIN_PROD` service account
  uses RSA key-pair auth, which the connector schema has no field for
  ([#994](https://github.com/oramasearch/zetta/issues/994), *Deferred*). If the
  conversation moves from his local pgvector to their real warehouse, that is the wall.
- **One methodology is executable.** The Pipeline compiler produces the tabular-supervised
  shape (M-1) plus part of the NLP/embeddings shape; the other nineteen in the catalogue
  are specified and not built ([zetta#984](https://github.com/oramasearch/zetta/issues/984)).
  Churn is exactly M-1, so this delivery is inside the built surface — but do not let the
  conversation drift to forecasting or uplift and assume the same answer.

The fuller accounting of what has been claimed to Charles versus what is true is
[`product-claims.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/projects/motive-fde-engagement/product-claims.md); read it before the Pat (CIO) presentation.

---

## 8 · Checklist for today

**Before sending anything:**

- [ ] Grant read on the four repos ([§0](#0--github-access--exactly-four-repos)); confirm
      his SSH key is on his GitHub account.
- [ ] Confirm the build is green from a clean clone on a machine that is not the dev box,
      and that the configure log says `Postgres source connector (libpqxx): ENABLED`
      ([§1](#1--clone-and-build)) — without it `type: postgres` fails at *query* time.
- [ ] **Decide which build he gets.** `v0.1.0` cannot do §4.1's connector path at all
      ([§4.2](#42--two-bugs-that-used-to-stop-41-dead--and-which-build-has-them)). Either
      ship him a build carrying [PR #996](https://github.com/oramasearch/zetta/pull/996),
      or agree in advance that the session uses the file path and the connector is not
      demoed. Verify with `./build/bin/zetta --help | grep -- --governance`.
- [ ] Dry-run §4.1 → §4.4 yourself on **his** shape of data, on **the build he will get**,
      before the session.
- [ ] **Correct the component list to one image** before he takes it to Tony
      ([§5b](#5b--do-we-ship-the-kubernetes-operator-too--no-and-that-is-good-news)) — the
      Aug 5 call said operator + Zetta, and the operator is both unnecessary today and the
      component most likely to fail a template review (cluster-scoped RBAC, its own CRDs).

**Send him:** this runbook, [`build-dependencies.md`](https://github.com/oramasearch/orama-bridge/blob/main/docs/projects/motive-fde-engagement/build-dependencies.md), and the
egress table in [§6.2](#62--the-build-time-egress-list--the-real-blocker) — the last of
those is the artifact for Tony, and **Tony leaves Charles's org on Aug 19**, so it should
go this week regardless of where the build gets to.

**Owed back to Charles, still open** (from the Aug 5 call):

- [ ] Model complexity → minimum target system benchmark, **with a cost column**. His
      machine is 32 GB M-series; he asked for it explicitly and it is not written yet.
- [ ] A no-privilege deployment mode — the product must work when it cannot provision DNS,
      cannot reach a cloud control plane, and cannot see its own database from outside.
      The operator/CRD surface demoed in July is precisely the surface Motive blocks.
- [ ] Usage/cost observability with budgets — he instruments this in his own work and
      expects it: *"whatever we build, I'm gonna need a usage observability node… and I
      even put budgets on it, so that it does not escalate on me."*
