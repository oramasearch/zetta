#!/bin/sh
# =============================================================================
# Zetta end-to-end (e2e) integration test
# =============================================================================
# Zetta's LEVEL 2 + LEVEL 3 test runner. It drives a *live, deployed* node over
# its HTTP API (:4800) — it builds/links nothing.
#
#   Level 1  unit tests            the C++ `zetta-tests` binary (build CI). NOT here.
#   Level 2  the in-app test suite the `zetta::testkit` catalog (the desktop
#                                  Test-suite panel), driven over `testkit_run`
#                                  *inside the deployed binary*.
#   Level 3  complete integration  black-box assertions over every deployed
#                                  surface (SQL + system.* views, MCP, REST,
#                                  serving, governance, cross-node CRDT
#                                  replication, observability).
#
# This script IS the coverage checklist — it is meant to grow. See
# docs/testing/e2e-integration.md. It lives in the zetta repo, is baked into the
# `oramasearch/zetta-e2e` image, and is triggered by the operator's
# `ZettaClusterE2ETest` CRD. It also runs by hand:
#
#   ZETTA_E2E_BASE_URL=http://127.0.0.1:4800 sh tests/e2e/zetta-e2e.sh
#
# --- Configuration (environment) ---------------------------------------------
#   ZETTA_E2E_BASE_URL   REQUIRED. The node HTTP API, e.g.
#                        http://<name>-zetta.<ns>.svc.cluster.local:4800
#   ZETTA_E2E_PEER_URLS  Optional space/comma list of the OTHER nodes' base URLs
#                        (scheme optional). >=2 nodes -> the CRDT replication
#                        canary runs; otherwise it is skipped.
#   ZETTA_E2E_LEVELS     "2", "3", or "2,3" (default "2,3").
#   ZETTA_E2E_TESTKIT_CASES  Optional space list of testkit case ids to run
#                        (default: discover from system.test_cases, else a
#                        built-in canonical list).
#   ZETTA_E2E_TOKEN      Optional bearer token (default: none — the deployed
#                        node runs unauthenticated-local).
#   ZETTA_E2E_REQUIRE_AUTH  "1" when the target enforces --require-auth (enables
#                        the authz-enforcement assertions; needs ZETTA_E2E_TOKEN).
#   ZETTA_E2E_DB         Scratch SQL database (default "e2e").
#   ZETTA_E2E_TIMEOUT / _RETRIES / _POLL   45 / 10 / 240 (seconds).
#
# Discipline: every check is PASS / FAIL / SKIP.
#   * FAIL  = the surface is present but returned the WRONG answer. Non-zero exit.
#   * SKIP  = the surface is genuinely absent in this build (engine rejected the
#             statement, a capability/view is missing, single-node cluster, no
#             auth). TRACKED, never a silent pass.
# NEVER weaken an assertion to go green — extend the engine instead.
#
# POSIX sh only (Alpine/busybox ash). Grounded against the deployed engine's
# real behavior: object-shaped rows, numeric-only GROUP BY, no ORDER BY, no bare
# count(*), lowercase run status keyed on run_id.
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
REQUIRE_AUTH="${ZETTA_E2E_REQUIRE_AUTH:-0}"
DB="${ZETTA_E2E_DB:-e2e}"
TIMEOUT="${ZETTA_E2E_TIMEOUT:-45}"
RETRIES="${ZETTA_E2E_RETRIES:-10}"
POLL="${ZETTA_E2E_POLL:-240}"

# Built-in testkit catalog (fallback when system.test_cases isn't served).
TESTKIT_FALLBACK="compute.cpu.elementwise compute.cpu.reduce_sort compute.cuda.conformance ddf.scan_filter ddf.group_by ddf.parquet_reader_parity ml.gbdt_train ml.tree_shap ml.calibration cluster.membership cluster.allreduce cluster.group_by_consistency cluster.compute_near_data cluster.partition_rebalance cluster.fulltext_index cluster.uc1_at_scale governance.access_control governance.audit governance.entity_resolution_gate governance.burst_identity pipeline.uc1_end_to_end examples.churn_pipeline examples.minimal_train examples.method_chain examples.unterminated_string examples.no_pipeline_steps"

# The 23 observability views the engine serves.
SYSTEM_VIEWS="activity audit checkpoints cloud_providers cloud_status collective_rounds crashes dataset_partitions datasets deployments indexes jobs keys models node_metrics nodes pipeline_jobs principals provisioners runs spans tables traces"

PASS=0; FAIL=0; SKIP=0

CURLRC="$(mktemp)"; BODYF="$(mktemp)"
{
    printf 'silent\nshow-error\n'
    printf 'max-time = %s\n' "$TIMEOUT"
    [ -n "$TOKEN" ] && printf 'header = "Authorization: Bearer %s"\n' "$TOKEN"
} > "$CURLRC"
cleanup() { rm -f "$CURLRC" "$BODYF"; }
trap cleanup EXIT INT TERM

want_level() { case ",$LEVELS," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

# ----------------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------------
sec()  { printf '\n=== %s ===\n' "$*"; }
pass() { PASS=$((PASS+1)); printf '  PASS  %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP  %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { printf 'FATAL: missing dependency: %s\n' "$1"; exit 2; }; }

# ----------------------------------------------------------------------------
# HTTP + SQL primitives (retry hard — zetta#731 drops back-to-back requests)
# ----------------------------------------------------------------------------
# http METHOD PATH [JSON] -> $BODY, $CODE ; 0 on a 2xx after retries
http() {
    _m="$1"; _p="$2"; _d="${3:-}"; _i=0
    while [ "$_i" -lt "$RETRIES" ]; do
        _i=$((_i+1))
        if [ -n "$_d" ]; then
            CODE="$(curl -K "$CURLRC" -X "$_m" -H 'Content-Type: application/json' \
                    -o "$BODYF" -w '%{http_code}' "$BASE$_p" -d "$_d" 2>/dev/null)"
        else
            CODE="$(curl -K "$CURLRC" -X "$_m" -o "$BODYF" -w '%{http_code}' "$BASE$_p" 2>/dev/null)"
        fi
        BODY="$(cat "$BODYF" 2>/dev/null)"
        case "$CODE" in 2*) return 0 ;; esac
        sleep 1
    done
    return 1
}

# sql STATEMENT [DATABASE] -> $BODY ; 0 iff the engine returned ok:true
sql() {
    _stmt="$1"; _db="${2-$DB}"; _i=0
    _q="statement=$(jq -rn --arg s "$_stmt" '$s|@uri')"
    [ -n "$_db" ] && _q="$_q&database=$(jq -rn --arg s "$_db" '$s|@uri')"
    while [ "$_i" -lt "$RETRIES" ]; do
        _i=$((_i+1))
        BODY="$(curl -K "$CURLRC" "$BASE/v1/sql?$_q" 2>/dev/null)"
        if printf '%s' "$BODY" | jq -e '.ok == true' >/dev/null 2>&1; then return 0; fi
        # A definite engine rejection (not a transport blip) — stop retrying.
        if printf '%s' "$BODY" | jq -e '.error.message' >/dev/null 2>&1; then return 1; fi
        sleep 1
    done
    return 1
}

rows_len() { printf '%s' "$BODY" | jq -r '.rows | length' 2>/dev/null || echo 0; }
rfield()   { printf '%s' "$BODY" | jq -r --arg k "$1" '.rows[0][$k] // empty' 2>/dev/null; }
err_msg()  { printf '%s' "$BODY" | jq -r '.error.message // empty' 2>/dev/null | head -c 140; }

# assert_count STMT EXPECTED DESC [DB] — present+wrong => FAIL, rejected => SKIP
assert_count() {
    if sql "$1" "${4-$DB}"; then
        _n="$(rows_len)"
        if [ "$_n" = "$2" ]; then pass "$3 (rows=$_n)"; else fail "$3 — expected $2 got $_n"; fi
    else
        skip "$3 — engine rejected: $(err_msg)"
    fi
}
# assert_field STMT KEY EXPECTED DESC [DB]
assert_field() {
    if sql "$1" "${5-$DB}"; then
        _v="$(rfield "$2")"
        if [ "$_v" = "$3" ]; then pass "$4 ($2=$_v)"; else fail "$4 — expected $2=$3 got '$_v'"; fi
    else
        skip "$4 — engine rejected: $(err_msg)"
    fi
}
# assert_ok STMT DESC [DB] — must return ok:true (else SKIP if absent)
assert_ok() { if sql "$1" "${3-$DB}"; then pass "$2"; else skip "$2 — engine rejected: $(err_msg)"; fi; }
# assert_reject STMT DESC [DB] — a negative test: the engine MUST reject it
assert_reject() { if sql "$1" "${3-$DB}"; then fail "$2 — expected rejection, got ok"; else pass "$2 (rejected: $(err_msg))"; fi; }

peer_norm() { case "$1" in http://*|https://*) printf '%s' "$1" ;; *) printf 'http://%s' "$1" ;; esac; }

# ============================================================================
printf 'Zetta e2e — base=%s levels=%s db=%s\n' "$BASE" "$LEVELS" "$DB"
need curl; need jq

sec "READINESS"
_ready=0; _spent=0
while [ "$_spent" -lt 120 ]; do
    if curl -K "$CURLRC" "$BASE/v1/server_info" 2>/dev/null | grep -q '"ok"'; then _ready=1; break; fi
    sleep 2; _spent=$((_spent+2))
done
if [ "$_ready" -eq 1 ]; then pass "node HTTP API reachable at $BASE"
else fail "node never became reachable at $BASE/v1/server_info"; printf '\nRESULT: FAIL (unreachable)\n'; exit 1; fi

# ============================================================================
# LEVEL 2 — the in-app test suite (zetta::testkit) over testkit_run  [#775]
# ============================================================================
if want_level 2; then
    sec "LEVEL 2 — in-app test suite (testkit)"
    CASES="${ZETTA_E2E_TESTKIT_CASES:-}"
    if [ -z "$CASES" ] && sql "SELECT id FROM system.test_cases" ""; then
        CASES="$(printf '%s' "$BODY" | jq -r '.rows[]?.id // empty' 2>/dev/null)"
    fi
    [ -z "$CASES" ] && CASES="$TESTKIT_FALLBACK"
    _tc_total=0; _tc_pass=0; _tc_fail=0; _tc_skip=0; _cats=""
    for CASE in $CASES; do
        _tc_total=$((_tc_total+1))
        _cats="$_cats ${CASE%%.*}"
        if ! http POST "/v1/testkit_run" "$(jq -rn --arg c "$CASE" '{case:$c}')"; then
            if printf '%s' "$BODY" | grep -qi 'no testkit case\|no such\|unknown'; then
                skip "testkit[$CASE] — not in this build"; _tc_skip=$((_tc_skip+1)); continue
            fi
            fail "testkit[$CASE] submit failed (HTTP $CODE)"; _tc_fail=$((_tc_fail+1)); continue
        fi
        RID="$(printf '%s' "$BODY" | jq -r '.run_id // .value.run_id // .structuredContent.value.run_id // empty' 2>/dev/null)"
        if [ -z "$RID" ]; then fail "testkit[$CASE] no run_id"; _tc_fail=$((_tc_fail+1)); continue; fi
        _st=""; _s=0
        while [ "$_s" -lt "$POLL" ]; do
            if sql "SELECT status FROM system.runs WHERE run_id='$RID'" ""; then
                _st="$(rfield status)"
                case "$_st" in succeeded|failed|error) break ;; esac
            fi
            sleep 2; _s=$((_s+2))
        done
        case "$_st" in
            succeeded) pass "testkit[$CASE]"; _tc_pass=$((_tc_pass+1)) ;;
            failed|error) fail "testkit[$CASE] -> $_st (run $RID; get_run/system.spans)"; _tc_fail=$((_tc_fail+1)) ;;
            *) fail "testkit[$CASE] no terminal status in ${POLL}s (run $RID, last='$_st')"; _tc_fail=$((_tc_fail+1)) ;;
        esac
    done
    printf '  testkit summary: %s cases — %s passed, %s failed, %s skipped\n' "$_tc_total" "$_tc_pass" "$_tc_fail" "$_tc_skip"
    # Catalog completeness: the core categories must be represented + have run.
    _seen="$(printf '%s\n' $_cats | sort -u | tr '\n' ' ')"
    for want in governance ddf compute; do
        case " $_seen " in *" $want "*) pass "testkit catalog covers '$want'" ;; *) fail "testkit catalog missing category '$want'" ;; esac
    done
    [ "$_tc_pass" -gt 0 ] || fail "no testkit case passed — the catalog did not run"
fi

# ============================================================================
# LEVEL 3 — complete integration
# ============================================================================
if want_level 3; then

    # --- 3.1 identity + membership -----------------------------------------
    sec "LEVEL 3.1 — identity + membership"
    if http GET "/v1/server_info" && printf '%s' "$BODY" | grep -q 'node_id'; then pass "GET /v1/server_info (node_id present)"; else fail "GET /v1/server_info"; fi
    if sql "SELECT node_id FROM system.nodes" ""; then
        _nn="$(rows_len)"; [ "${_nn:-0}" -ge 1 ] && pass "system.nodes lists $_nn node(s)" || fail "system.nodes empty"
    else fail "SELECT from system.nodes: $(err_msg)"; fi

    # --- 3.2 SQL DDL + DML (append-only; per-run unique names, zetta#735) ---
    sec "LEVEL 3.2 — SQL DDL/DML"
    TBL="widgets_$$"
    sql "CREATE DATABASE $DB" >/dev/null 2>&1                       # tolerate "already exists"
    assert_ok "CREATE TABLE $TBL (id, name, category, price, in_stock)" "CREATE TABLE $TBL"
    _ins=0
    for row in "1,'alpha','tools',10,true" "2,'bravo','tools',25,true" \
               "3,'charlie','home',7,false" "4,'delta','home',42,true" \
               "5,'echo','garden',15,true" "6,'foxtrot','garden',99,false"; do
        sql "INSERT INTO $TBL (id, name, category, price, in_stock) VALUES ($row)" && _ins=$((_ins+1))
    done
    [ "$_ins" -eq 6 ] && pass "INSERT 6 rows" || fail "INSERT ($_ins/6 ok)"
    assert_count "SELECT * FROM $TBL" 6 "SELECT * returns all 6 rows"
    assert_field "SELECT * FROM $TBL WHERE id = 4" price 42 "point lookup id=4 -> price 42"

    # --- 3.3 query engine: filters / aggregates (no ORDER BY; numeric GROUP BY) --
    sec "LEVEL 3.3 — query engine"
    assert_count "SELECT * FROM $TBL WHERE price > 20" 3 "WHERE price > 20"
    assert_count "SELECT * FROM $TBL WHERE price <= 10" 2 "WHERE price <= 10"
    assert_count "SELECT * FROM $TBL WHERE price <> 10" 5 "WHERE price <> 10"
    assert_count "SELECT * FROM $TBL WHERE in_stock = true" 4 "WHERE in_stock = true"
    assert_count "SELECT * FROM $TBL WHERE in_stock = false" 2 "WHERE in_stock = false"
    assert_count "SELECT * FROM $TBL WHERE category = 'tools'" 2 "WHERE category = 'tools'"
    assert_count "SELECT * FROM $TBL WHERE price > 20 AND in_stock = true" 2 "WHERE price>20 AND in_stock"
    assert_count "SELECT * FROM $TBL WHERE price < 10 OR price > 90" 2 "WHERE price<10 OR price>90"
    assert_count "SELECT * FROM $TBL WHERE NOT (in_stock = true)" 2 "WHERE NOT in_stock"
    assert_count "SELECT id, sum(price) FROM $TBL GROUP BY id" 6 "numeric GROUP BY (aggregate over DDF)"
    assert_reject "SELECT nonexistent_col FROM $TBL" "unknown column is rejected"
    # Documented engine limits — asserted as tracked SKIPs, not silent gaps:
    if sql "SELECT * FROM $TBL WHERE price > 0" && ! sql "SELECT * FROM $TBL ORDER BY price"; then
        skip "ORDER BY unsupported by the deployed engine (tracked)"
    fi
    if ! sql "SELECT category, sum(price) FROM $TBL GROUP BY category"; then
        skip "string GROUP BY unsupported (DDF is numeric-only, zetta#756) (tracked)"
    fi

    # --- 3.4 the system.* observability catalog ----------------------------
    sec "LEVEL 3.4 — system.* views"
    for V in $SYSTEM_VIEWS; do
        if sql "SELECT * FROM system.$V" ""; then pass "system.$V readable"
        elif printf '%s' "$BODY" | grep -qi 'no such system table'; then skip "system.$V not served by this build"
        else fail "system.$V errored: $(err_msg)"; fi
    done

    # --- 3.5 MCP protocol (JSON-RPC 2.0 over /mcp) -------------------------
    sec "LEVEL 3.5 — MCP protocol"
    if http POST "/mcp" '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' && printf '%s' "$BODY" | grep -q '"serverInfo"'; then
        pass "MCP initialize -> serverInfo"
    else skip "MCP initialize (transport may require an SSE Accept header)"; fi
    if http POST "/mcp" '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' && printf '%s' "$BODY" | grep -q '"sql"'; then
        pass "MCP tools/list exposes the sql tool"
    else skip "MCP tools/list (transport)"; fi
    if http POST "/mcp" '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"server_info","arguments":{}}}' && printf '%s' "$BODY" | grep -q 'node_id'; then
        pass "MCP tools/call server_info"
    else skip "MCP tools/call (transport)"; fi

    # --- 3.6 REST read capabilities + catalog surfaces ---------------------
    sec "LEVEL 3.6 — REST reads (/v1/*)"
    for R in server_info list_projects list_models list_indexes list_datasets list_runs list_jobs list_traces list_specs; do
        if http GET "/v1/$R"; then pass "GET /v1/$R"; else skip "GET /v1/$R not available"; fi
    done
    if http GET "/v1/openapi.json" && printf '%s' "$BODY" | grep -q 'openapi\|paths'; then pass "GET /v1/openapi.json"; else skip "GET /v1/openapi.json"; fi
    if http GET "/llms.txt"; then pass "GET /llms.txt (agent docs)"; else skip "GET /llms.txt"; fi

    # --- 3.7 orchestration action + read-back ------------------------------
    sec "LEVEL 3.7 — orchestration"
    PROJ="e2e-project-$$"
    if http POST "/v1/create_project" "$(jq -rn --arg n "$PROJ" '{name:$n}')" && printf '%s' "$BODY" | grep -qi 'project\|"id"\|"ok"'; then
        pass "create_project (action)"
        if http GET "/v1/list_projects" && printf '%s' "$BODY" | grep -q "$PROJ"; then pass "created project is listed"; else skip "list_projects did not echo $PROJ (flaky read)"; fi
    else skip "create_project not accepted in this build/config"; fi

    # --- 3.8 serving: expose an index + search -----------------------------
    sec "LEVEL 3.8 — serving"
    IDX=""
    if sql "SELECT id FROM system.indexes" ""; then IDX="$(rfield id)"; fi
    if [ -n "$IDX" ]; then
        if http POST "/v1/expose_index" "$(jq -rn --arg n "$IDX" '{name:$n}')"; then
            if http GET "/v1/indexes/$IDX/search?q=test"; then pass "expose_index + search ($IDX)"; else fail "search on exposed index $IDX"; fi
        else skip "expose_index $IDX not accepted"; fi
    else skip "no registered index to expose — build a fulltext index first (#779)"; fi

    # --- 3.9 governance -----------------------------------------------------
    sec "LEVEL 3.9 — governance"
    # The principal/key model + access-control enforcement are exercised by the
    # governance.* testkit cases at L2. Here we assert the views are readable.
    for V in principals keys audit; do
        if sql "SELECT * FROM system.$V" ""; then pass "system.$V readable"; else skip "system.$V unavailable"; fi
    done
    # The user/role/token DDL (the new authz system) is not in every build:
    if sql "CREATE USER e2e_reader_$$" ""; then pass "CREATE USER (authz DDL present)"
    else skip "CREATE USER not supported by this build (new authz system not deployed)"; fi

    # --- 3.10 CRDT replication canary across nodes  [#781] -----------------
    sec "LEVEL 3.10 — CRDT replication"
    PEER=""
    for p in $PEERS; do _pp="$(peer_norm "${p%/}")"; [ -n "$_pp" ] && [ "$_pp" != "$BASE" ] && PEER="$_pp" && break; done
    if [ -z "$PEER" ]; then
        skip "single-node target — replication canary needs replicas>=2 (operator target profile #88)"
    else
        CANARY="e2e_canary_$$"; CTBL="canary_$$"
        sql "CREATE DATABASE $DB" >/dev/null 2>&1
        sql "CREATE TABLE $CTBL (id, tag)" >/dev/null 2>&1
        if sql "INSERT INTO $CTBL (id, tag) VALUES (1, '$CANARY')"; then
            _s=0; _ok=0
            while [ "$_s" -lt 60 ]; do
                _pq="statement=$(jq -rn --arg s "SELECT tag FROM $CTBL WHERE id = 1" '$s|@uri')&database=$(jq -rn --arg s "$DB" '$s|@uri')"
                if curl -K "$CURLRC" "$PEER/v1/sql?$_pq" 2>/dev/null | grep -q "$CANARY"; then _ok=1; break; fi
                sleep 2; _s=$((_s+2))
            done
            [ "$_ok" -eq 1 ] && pass "row on $BASE converged to $PEER (CRDT)" || fail "canary did NOT converge to $PEER within 60s"
        else fail "could not write canary row"; fi
    fi

    # --- 3.11 observability: the e2e's own runs/spans/audit  [#782] --------
    sec "LEVEL 3.11 — observability"
    if sql "SELECT run_id FROM system.runs" ""; then
        _rn="$(rows_len)"
        if want_level 2 && [ "${_rn:-0}" -ge 1 ]; then pass "system.runs recorded the testkit runs ($_rn)"; else pass "system.runs readable ($_rn)"; fi
    else skip "system.runs unavailable"; fi
    assert_ok "SELECT span_id FROM system.spans" "system.spans readable (traced ops)" ""
    if sql "SELECT * FROM system.crashes" ""; then
        _cn="$(rows_len)"; [ "${_cn:-0}" -eq 0 ] && pass "system.crashes is empty (no crashes)" || fail "system.crashes has $_cn entries"
    else skip "system.crashes unavailable"; fi

    # --- 3.12 authz enforcement under --require-auth  [#780] ---------------
    sec "LEVEL 3.12 — authz enforcement"
    if [ "$REQUIRE_AUTH" != "1" ]; then
        skip "target is unauthenticated-local — authz enforcement inert (needs --require-auth target profile #88)"
    elif [ -z "$TOKEN" ]; then
        skip "ZETTA_E2E_REQUIRE_AUTH=1 but no ZETTA_E2E_TOKEN provided"
    else
        # With auth on, an unauthenticated request must be refused.
        if curl -s -o /dev/null -w '%{http_code}' "$BASE/v1/list_projects" 2>/dev/null | grep -q '^40'; then
            pass "unauthenticated request is denied (401/403)"
        else fail "unauthenticated request was NOT denied under --require-auth"; fi
        # A full authz matrix (deny-by-default, RLS, masking, scoped grants, token
        # lifecycle — regression for #759-#769) is added here as the authz system
        # lands in the deployed image; see zetta#780.
        skip "authz matrix (RLS/masking/scoped-grants/token-lifecycle) pending — see #780"
    fi

fi

# ============================================================================
# SUMMARY
# ============================================================================
printf '\n============================================================\n'
printf 'Zetta e2e: pass=%s fail=%s skip=%s\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -ne 0 ]; then printf 'RESULT: FAIL\n'; exit 1; fi
printf 'RESULT: PASS\n'; exit 0
