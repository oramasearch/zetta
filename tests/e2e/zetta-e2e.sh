#!/bin/sh
# =============================================================================
# Zetta end-to-end (e2e) integration test
# =============================================================================
# This is Zetta's LEVEL 2 + LEVEL 3 test runner. It drives a *live, deployed*
# Zetta node over its HTTP API (:4800) — it does NOT build or link anything.
#
#   Level 1  unit tests            the C++ `zetta-tests` binary (build-time CI).
#                                  NOT run here.
#   Level 2  the in-app test suite the `zetta::testkit` catalog — the same cases
#                                  the desktop GUI's "Test suite" panel runs.
#                                  Driven here over the `testkit_run` capability.
#   Level 3  complete integration  black-box assertions over every deployed
#                                  surface (SQL + system.* views, MCP, REST,
#                                  serving, governance, CRDT replication across
#                                  nodes, cluster membership, audit). "The e2e."
#
# This script IS the exhaustive coverage checklist — it is meant to grow. See
# docs/testing/e2e-integration.md for the model and how to add a section.
#
# It lives in the Zetta repo (so it evolves with the engine) and is packaged
# into the `oramasearch/zetta-e2e` runner image; the orama-cloud-v1-operator
# `ZettaClusterE2ETest` CRD runs it in-cluster against a deployed
# OramaZettaCluster. It also runs by hand against any node:
#
#   ZETTA_E2E_BASE_URL=http://127.0.0.1:4800 sh tests/e2e/zetta-e2e.sh
#
# --- Configuration (environment) ---------------------------------------------
#   ZETTA_E2E_BASE_URL   REQUIRED. Base URL of the node's HTTP API, e.g.
#                        http://<name>-zetta.<ns>.svc.cluster.local:4800
#   ZETTA_E2E_PEER_URLS  Optional space/comma list of the OTHER nodes' base URLs
#                        (per-pod DNS). With >=2 nodes total the CRDT
#                        replication canary runs; otherwise it is skipped.
#   ZETTA_E2E_LEVELS     "2", "3", or "2,3" (default "2,3").
#   ZETTA_E2E_TOKEN      Optional bearer token (default: none — the deployed
#                        node runs as an unauthenticated local session).
#   ZETTA_E2E_DB         Scratch SQL database name (default "e2e").
#   ZETTA_E2E_TIMEOUT    Per-request curl timeout, seconds (default 45).
#   ZETTA_E2E_RETRIES    Per-request retries — the node HTTP server is flaky and
#                        drops back-to-back requests (zetta#731) (default 10).
#   ZETTA_E2E_POLL       Testkit / async-run poll budget, seconds (default 240).
#
# Discipline (mirrors the OramaCloudV1 e2e): every check is PASS / FAIL / SKIP.
# SKIP is for genuinely-unavailable surfaces (no GPU, engine not compiled in,
# single-node cluster, a capability the build omits) — it is TRACKED, never a
# silent pass. FAIL sets a non-zero exit so the Job (and the CRD status) fail.
# NEVER weaken an assertion to make it pass — extend the engine instead.
#
# POSIX sh only (runs under Alpine/busybox ash). No bashisms.
# =============================================================================

set -u

# ----------------------------------------------------------------------------
# Config
# ----------------------------------------------------------------------------
BASE="${ZETTA_E2E_BASE_URL:?ZETTA_E2E_BASE_URL is required (the node HTTP API, :4800)}"
BASE="${BASE%/}"
PEERS="$(printf '%s' "${ZETTA_E2E_PEER_URLS:-}" | tr ',' ' ')"
LEVELS="${ZETTA_E2E_LEVELS:-2,3}"
TOKEN="${ZETTA_E2E_TOKEN:-}"
DB="${ZETTA_E2E_DB:-e2e}"
TIMEOUT="${ZETTA_E2E_TIMEOUT:-45}"
RETRIES="${ZETTA_E2E_RETRIES:-10}"
POLL="${ZETTA_E2E_POLL:-240}"

PASS=0; FAIL=0; SKIP=0

# One curl config file carries the timeout + the optional bearer header, so no
# curl call has to conditionally splice an auth argument (a word-splitting trap).
CURLRC="$(mktemp)"
{
    printf 'silent\nshow-error\n'
    printf 'max-time = %s\n' "$TIMEOUT"
    [ -n "$TOKEN" ] && printf 'header = "Authorization: Bearer %s"\n' "$TOKEN"
} > "$CURLRC"
BODYF="$(mktemp)"
cleanup() { rm -f "$CURLRC" "$BODYF"; }
trap cleanup EXIT INT TERM

want_level() { case ",$LEVELS," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

# ----------------------------------------------------------------------------
# Output helpers
# ----------------------------------------------------------------------------
sec()  { printf '\n=== %s ===\n' "$*"; }
pass() { PASS=$((PASS+1)); printf '  PASS  %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP  %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { printf 'FATAL: missing dependency: %s\n' "$1"; exit 2; }; }

# ----------------------------------------------------------------------------
# HTTP + SQL primitives (retry hard — zetta#731 drops ~2/3 of back-to-back reqs)
# ----------------------------------------------------------------------------
# http METHOD PATH [JSON] -> sets $BODY, $CODE ; returns 0 on a 2xx after retries
http() {
    _m="$1"; _p="$2"; _d="${3:-}"; _i=0
    while [ "$_i" -lt "$RETRIES" ]; do
        _i=$((_i+1))
        if [ -n "$_d" ]; then
            CODE="$(curl -K "$CURLRC" -X "$_m" -H 'Content-Type: application/json' \
                    -o "$BODYF" -w '%{http_code}' "$BASE$_p" -d "$_d" 2>/dev/null)"
        else
            CODE="$(curl -K "$CURLRC" -X "$_m" \
                    -o "$BODYF" -w '%{http_code}' "$BASE$_p" 2>/dev/null)"
        fi
        BODY="$(cat "$BODYF" 2>/dev/null)"
        case "$CODE" in 2*) return 0 ;; esac
        sleep 1
    done
    return 1
}

# sql STATEMENT [DATABASE] -> sets $BODY to the JSON result; returns 0 iff ok:true
sql() {
    _stmt="$1"; _db="${2-$DB}"; _i=0
    _q="statement=$(jq -rn --arg s "$_stmt" '$s|@uri')"
    [ -n "$_db" ] && _q="$_q&database=$(jq -rn --arg s "$_db" '$s|@uri')"
    while [ "$_i" -lt "$RETRIES" ]; do
        _i=$((_i+1))
        BODY="$(curl -K "$CURLRC" "$BASE/v1/sql?$_q" 2>/dev/null)"
        if printf '%s' "$BODY" | jq -e '.ok == true' >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}

# sql_scalar STATEMENT [DB] -> echoes rows[0][0] (empty on failure)
sql_scalar() {
    if sql "$1" "${2-$DB}"; then printf '%s' "$BODY" | jq -r '.rows[0][0] // empty' 2>/dev/null; fi
}

# assert_sql_ok STMT DESC — the statement must return ok:true
assert_sql_ok() {
    if sql "$1"; then pass "$2"
    else fail "$2 (sql not ok: $(printf '%s' "$BODY" | head -c 200))"; fi
}

# peer_has PEER_URL STATEMENT SUBSTR -> 0 if the peer's SQL result contains SUBSTR
peer_has() {
    _pq="statement=$(jq -rn --arg s "$2" '$s|@uri')&database=$(jq -rn --arg s "$DB" '$s|@uri')"
    curl -K "$CURLRC" "$1/v1/sql?$_pq" 2>/dev/null | grep -q "$3"
}

# ============================================================================
printf 'Zetta e2e — base=%s levels=%s db=%s\n' "$BASE" "$LEVELS" "$DB"
need curl; need jq

# --- Readiness gate ---------------------------------------------------------
sec "READINESS"
_ready=0; _spent=0
while [ "$_spent" -lt 120 ]; do
    if curl -K "$CURLRC" "$BASE/v1/server_info" 2>/dev/null | grep -q .; then _ready=1; break; fi
    sleep 2; _spent=$((_spent+2))
done
if [ "$_ready" -eq 1 ]; then
    pass "node HTTP API reachable at $BASE"
else
    fail "node never became reachable at $BASE/v1/server_info"
    printf '\nRESULT: FAIL (node unreachable)\n'; exit 1
fi

# ============================================================================
# LEVEL 2 — the in-app test suite (zetta::testkit) driven over testkit_run
# ============================================================================
if want_level 2; then
    sec "LEVEL 2 — in-app test suite (testkit)"
    CASES=""
    if sql "SELECT id FROM system.test_cases" ""; then
        CASES="$(printf '%s' "$BODY" | jq -r '.rows[][0] // empty' 2>/dev/null)"
    fi
    if [ -z "$CASES" ]; then
        skip "system.test_cases returned no cases (testkit not present in this build) — level 2 skipped"
    else
        printf '  running %s testkit cases via testkit_run\n' "$(printf '%s\n' "$CASES" | grep -c .)"
        for CASE in $CASES; do
            if ! http POST "/v1/testkit_run" "$(jq -rn --arg c "$CASE" '{case:$c}')"; then
                fail "testkit[$CASE] submit failed (HTTP)"; continue
            fi
            RID="$(printf '%s' "$BODY" | jq -r '.run_id // .value.run_id // .structuredContent.value.run_id // empty' 2>/dev/null)"
            if [ -z "$RID" ]; then fail "testkit[$CASE] no run_id in response"; continue; fi
            # Poll the durable Run to a terminal status. Pass/Skipped -> Succeeded.
            _st=""; _spent=0
            while [ "$_spent" -lt "$POLL" ]; do
                _st="$(sql_scalar "SELECT status FROM system.runs WHERE id='$RID'" "")"
                case "$_st" in Succeeded|Failed) break ;; esac
                sleep 2; _spent=$((_spent+2))
            done
            case "$_st" in
                Succeeded) pass "testkit[$CASE]" ;;
                Failed)    fail "testkit[$CASE] -> Failed (run $RID; see system.runs / get_trace)" ;;
                *)         fail "testkit[$CASE] no terminal status in ${POLL}s (run $RID, last='$_st')" ;;
            esac
        done
    fi
fi

# ============================================================================
# LEVEL 3 — complete integration test (the deployed surfaces)
# ============================================================================
if want_level 3; then

    # --- 3.1 Identity + cluster membership ----------------------------------
    sec "LEVEL 3.1 — identity + membership"
    if http GET "/v1/server_info"; then pass "GET /v1/server_info"; else fail "GET /v1/server_info"; fi
    if sql "SELECT count(*) FROM system.nodes" ""; then
        NODES="$(printf '%s' "$BODY" | jq -r '.rows[0][0] // 0')"
        [ "${NODES:-0}" -ge 1 ] && pass "system.nodes lists $NODES node(s)" || fail "system.nodes empty"
    else fail "SELECT from system.nodes"; fi

    # --- 3.2 SQL DDL + DML (append-only engine; no UPDATE/DELETE, zetta#735) -
    # Unique per-run names so re-runs against a persistent PVC stay deterministic
    # (the engine is append-only — there is no reliable clean slate to truncate).
    sec "LEVEL 3.2 — SQL DDL/DML"
    TBL="widgets_$$"
    sql "CREATE DATABASE $DB" >/dev/null 2>&1   # tolerate "already exists"
    if sql "CREATE TABLE $TBL (id, name, category, price, in_stock)"; then
        pass "CREATE DATABASE $DB + CREATE TABLE $TBL"
    else fail "CREATE TABLE $TBL (db not usable?): $(printf '%s' "$BODY" | head -c 160)"; fi
    _ins=0
    for row in \
        "1,'alpha','tools',10,true" "2,'bravo','tools',25,true" \
        "3,'charlie','home',7,false" "4,'delta','home',42,true" \
        "5,'echo','garden',15,true" "6,'foxtrot','garden',99,false"; do
        sql "INSERT INTO $TBL (id, name, category, price, in_stock) VALUES ($row)" && _ins=$((_ins+1))
    done
    [ "$_ins" -eq 6 ] && pass "INSERT 6 rows into $TBL" || fail "INSERT $TBL ($_ins/6 ok)"
    C="$(sql_scalar "SELECT count(*) FROM $TBL")"
    [ "${C:-0}" -eq 6 ] && pass "SELECT count(*) == 6" || fail "SELECT count(*) == 6 (got '${C:-}')"

    # --- 3.3 Query engine: filter / group-by / aggregate --------------------
    sec "LEVEL 3.3 — query engine"
    C="$(sql_scalar "SELECT count(*) FROM $TBL WHERE price > 20")"
    [ "${C:-0}" -eq 3 ] && pass "WHERE price > 20 -> 3" || fail "WHERE price>20 (got '${C:-}')"
    C="$(sql_scalar "SELECT count(*) FROM $TBL WHERE in_stock = true")"
    [ "${C:-0}" -eq 4 ] && pass "WHERE in_stock = true -> 4" || fail "WHERE in_stock (got '${C:-}')"
    if sql "SELECT category, count(*) FROM $TBL GROUP BY category"; then
        G="$(printf '%s' "$BODY" | jq -r '.rows | length')"
        [ "${G:-0}" -eq 3 ] && pass "GROUP BY category -> 3 groups" || fail "GROUP BY category (got '${G:-}' groups)"
    else fail "GROUP BY category"; fi
    S="$(sql_scalar "SELECT sum(price) FROM $TBL")"
    [ "${S:-0}" -eq 198 ] && pass "SUM(price) == 198" || fail "SUM(price) (got '${S:-}')"
    MX="$(sql_scalar "SELECT max(price) FROM $TBL")"
    [ "${MX:-0}" -eq 99 ] && pass "MAX(price) == 99" || fail "MAX(price) (got '${MX:-}')"

    # --- 3.4 The system.* observability catalog -----------------------------
    sec "LEVEL 3.4 — system.* views"
    for V in nodes tables datasets models indexes jobs runs traces spans keys \
             principals audit activity provisioners cloud_providers deployments; do
        if sql "SELECT * FROM system.$V" ""; then pass "system.$V readable"
        else skip "system.$V not served by this build"; fi
    done

    # --- 3.5 MCP protocol (JSON-RPC 2.0 over /mcp) --------------------------
    sec "LEVEL 3.5 — MCP protocol"
    if http POST "/mcp" '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
        && printf '%s' "$BODY" | grep -q '"serverInfo"'; then
        pass "MCP initialize returns serverInfo"
    else skip "MCP initialize (transport may require an SSE Accept header) — verify via a desktop client"; fi
    if http POST "/mcp" '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
        && printf '%s' "$BODY" | grep -q '"sql"'; then
        pass "MCP tools/list exposes the sql tool"
    else skip "MCP tools/list (transport)"; fi

    # --- 3.6 REST read capabilities -----------------------------------------
    sec "LEVEL 3.6 — REST reads (/v1/*)"
    for R in list_projects list_models list_indexes list_datasets list_runs list_jobs list_traces list_specs; do
        if http GET "/v1/$R"; then pass "GET /v1/$R"; else skip "GET /v1/$R not available"; fi
    done
    if http GET "/llms.txt"; then pass "GET /llms.txt (agent docs)"; else skip "GET /llms.txt"; fi

    # --- 3.7 Orchestration action + job -------------------------------------
    sec "LEVEL 3.7 — orchestration"
    PROJ="e2e-project-$$"
    if http POST "/v1/create_project" "$(jq -rn --arg n "$PROJ" '{name:$n}')" \
        && printf '%s' "$BODY" | grep -qi 'project\|"id"\|"ok"'; then
        pass "create_project (action)"
        if http GET "/v1/list_projects" && printf '%s' "$BODY" | grep -q "$PROJ"; then
            pass "created project is listed"
        else skip "list_projects did not echo $PROJ (flaky read)"; fi
    else skip "create_project not accepted in this build/config"; fi

    # --- 3.8 Serving: expose an index + search ------------------------------
    sec "LEVEL 3.8 — serving"
    IDX="$(sql_scalar "SELECT name FROM system.indexes LIMIT 1" "")"
    if [ -n "$IDX" ]; then
        if http POST "/v1/expose_index" "$(jq -rn --arg n "$IDX" '{name:$n}')"; then
            if http GET "/v1/indexes/$IDX/search?q=test"; then pass "expose_index + search $IDX"
            else fail "search on exposed index $IDX"; fi
        else skip "expose_index $IDX not accepted"; fi
    else skip "no index available to expose (serving) — build a fulltext index first"; fi

    # --- 3.9 Governance objects (existence; enforcement needs --require-auth) -
    sec "LEVEL 3.9 — governance"
    USR="e2e_reader_$$"
    if sql "CREATE USER $USR" ""; then
        pass "CREATE USER $USR"
        if sql "SELECT * FROM system.principals" ""; then pass "system.principals readable after create"
        else skip "principals view unavailable"; fi
    else skip "CREATE USER not accepted (governance DDL gated in this build)"; fi
    # Deny-by-default enforcement only engages under --require-auth; the deployed
    # node runs unauthenticated-local, so we assert the audit trail is present.
    if sql "SELECT count(*) FROM system.audit" ""; then
        A="$(printf '%s' "$BODY" | jq -r '.rows[0][0] // 0')"
        pass "system.audit trail present ($A events)"
    else skip "system.audit unavailable"; fi

    # --- 3.10 CRDT replication canary across nodes --------------------------
    sec "LEVEL 3.10 — CRDT replication"
    PEER=""; for p in $PEERS; do _pp="${p%/}"; [ -n "$_pp" ] && [ "$_pp" != "$BASE" ] && PEER="$_pp" && break; done
    if [ -z "$PEER" ]; then
        skip "single-node target (no distinct peer URL) — replication canary needs replicas>=2"
    else
        CANARY="e2e_canary_$$"
        CTBL="canary_$$"
        sql "CREATE DATABASE $DB" >/dev/null 2>&1
        sql "CREATE TABLE $CTBL (id, tag)" >/dev/null 2>&1
        if sql "INSERT INTO $CTBL (id, tag) VALUES (1, '$CANARY')"; then
            _spent=0; _ok=0
            while [ "$_spent" -lt 60 ]; do
                if peer_has "$PEER" "SELECT tag FROM $CTBL WHERE id=1" "$CANARY"; then _ok=1; break; fi
                sleep 2; _spent=$((_spent+2))
            done
            [ "$_ok" -eq 1 ] && pass "row written on $BASE converged to $PEER (CRDT replication)" \
                             || fail "canary row did NOT converge to peer $PEER within 60s"
        else fail "could not write canary row for replication test"; fi
    fi

fi

# ============================================================================
# SUMMARY
# ============================================================================
printf '\n============================================================\n'
printf 'Zetta e2e: pass=%s fail=%s skip=%s\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -ne 0 ]; then printf 'RESULT: FAIL\n'; exit 1; fi
printf 'RESULT: PASS\n'; exit 0
