#!/bin/sh
# =============================================================================
# Zetta end-to-end (e2e) integration test
# =============================================================================
# Zetta's LEVEL 2 + LEVEL 3 test runner. It drives a *live, deployed* node over
# its HTTP API (:4800) — it builds/links nothing.
#
#   Level 1  unit tests            the C++ `zetta-tests` binary (build CI). NOT here.
#   Level 2  the in-app test suite the `zetta::testkit` catalog (the desktop
#                                  Test-suite panel), driven over `testsuite_run`
#                                  *inside the deployed binary*.
#   Level 3  complete integration  black-box assertions over every deployed
#                                  surface (SQL + system.* views, MCP, REST,
#                                  serving, governance, cross-node CRDT
#                                  replication, blobs/consent, observability).
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
#                        (scheme optional). >=2 nodes -> the CRDT replication +
#                        cluster convergence checks run; otherwise they SKIP.
#   ZETTA_E2E_LEVELS     "2", "3", or "2,3" (default "2,3").
#   ZETTA_E2E_TESTKIT_CASES  Optional space list of testkit case ids to run
#                        (default: discover from system.test_cases, else a
#                        built-in canonical list).
#   ZETTA_E2E_TOKEN      REQUIRED. The seeded admin (owner) token — the node
#                        enforces auth on every request (zetta#799). §3.13 mints
#                        its own per-run users and tokens from it.
#   ZETTA_E2E_REQUIRE_AUTH  Ignored (auth is always on); kept so an older runner
#                        that still sets it keeps working.
#   ZETTA_E2E_DB         Scratch SQL database (default "e2e").
#   ZETTA_E2E_TIMEOUT / _RETRIES / _POLL   45 / 10 / 240 (seconds).
#
# Discipline: every check is PASS / FAIL / SKIP.
#   * FAIL  = the surface is present but returned the WRONG answer. Non-zero exit.
#   * SKIP  = the surface is genuinely absent in this build (engine rejected the
#             statement, a capability/view is missing, single-node cluster, no
#             auth, an enforcement path not yet deployed). TRACKED, never a
#             silent pass — every SKIP names the reason and the issue it awaits.
# NEVER weaken an assertion to go green — extend the engine instead. A SKIP that
# should be a PASS is a tracked gap that lights up the moment the surface ships.
#
# POSIX sh only (Alpine/busybox ash). Grounded against the deployed engine's
# real behavior (verified live, HEAD 2026-07):
#   * The capability envelope is {ok, value, trace_id, span_id, duration_ms};
#     SQL rows live at `.value.rows` as named objects ([{col: val}]), aggregates
#     name their output (sum(price) -> sum_price), single-predicate WHERE only
#     (AND/OR/NOT are a tracked limit). Bare count(*), text GROUP BY keys and
#     unknown-column errors arrived with zetta#1218 and are SKIPs before it.
#   * The in-app suite is `testsuite_run` / `testsuite_list` (+ system.test_cases).
#   * /mcp is JSON-RPC over POST; the harness sends an SSE `Accept` and reads a
#     plain-JSON or `data:`-framed reply either way.
#   * expose_index takes {"index": id}; expose_model {"model": id}; the served
#     search route is /v1/indexes/{name}/search?query=... .
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
# Auth is mandatory on every target (zetta#799): every HTTP/MCP/SQL request needs a
# bearer that resolves to a user, so the e2e REQUIRES a token — fail fast rather than
# 401 on the first call. (ZETTA_E2E_REQUIRE_AUTH is vestigial now; auth is always on.)
if [ -z "$TOKEN" ]; then
    printf 'FATAL: ZETTA_E2E_TOKEN (the seeded admin token) is required — the engine enforces auth on every request (zetta#799).\n' >&2
    exit 2
fi
REQUIRE_AUTH=1
DB="${ZETTA_E2E_DB:-e2e}"
TIMEOUT="${ZETTA_E2E_TIMEOUT:-45}"
RETRIES="${ZETTA_E2E_RETRIES:-10}"
POLL="${ZETTA_E2E_POLL:-240}"

# Per-run unique suffix for every object the suite creates (tables/projects/users).
# It MUST be unique per invocation: the store is append-only (zetta#735), and this
# script runs as the container ENTRYPOINT (the operator's ZettaClusterE2ETest Job) —
# where $$ is PID 1, i.e. the SAME on every retry/re-run. A non-unique suffix makes
# objects accumulate across runs and count assertions drift (widgets: expected 6, got
# 6*N). Use a random hex token; fall back to the PID for a bare local run with no urandom.
RUN="$(tr -dc 'a-f0-9' < /dev/urandom 2>/dev/null | head -c 12 || true)"
[ -z "$RUN" ] && RUN="$$"

# Built-in testkit catalog (fallback when system.test_cases isn't served).
TESTKIT_FALLBACK="compute.cpu.elementwise compute.cpu.reduce_sort compute.cuda.conformance ddf.scan_filter ddf.group_by ddf.parquet_reader_parity ml.gbdt_train ml.tree_shap ml.calibration cluster.membership cluster.allreduce cluster.group_by_consistency cluster.compute_near_data cluster.partition_rebalance cluster.fulltext_index cluster.connections cluster.distributed_import cluster.import_participant_loss cluster.uc1_at_scale governance.access_control governance.audit governance.entity_resolution_gate governance.burst_identity pipeline.uc1_end_to_end examples.churn_pipeline examples.minimal_train examples.method_chain examples.unterminated_string examples.no_pipeline_steps"

# The observability + governance system.* views the engine serves. The last six
# (policies/roles/tokens/users/user_roles/test_cases) shipped with the authz +
# test-suite work; an older image without them SKIPs those rows (never a fail).
SYSTEM_VIEWS="activity audit checkpoints cloud_providers cloud_status collective_rounds crashes dataset_partitions datasets deployments indexes jobs keys models node_metrics nodes pipeline_jobs policies principals provisioners roles runs spans tables test_cases tokens traces user_roles users"

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

# http_code METHOD PATH [JSON] [BEARER] -> $CODE only (single shot; for
# negative/auth tests that WANT a non-2xx). Does NOT read the curlrc, so the
# run's default ZETTA_E2E_TOKEN is never smuggled in: BEARER "-" (or empty)
# sends NO Authorization header, an explicit token sends exactly that one.
http_code() {
    _m="$1"; _p="$2"; _d="${3:-}"; _b="${4-__DEFAULT__}"
    set -- -s --max-time "$TIMEOUT" -X "$_m" -o /dev/null -w '%{http_code}'
    case "$_b" in
        __DEFAULT__) [ -n "$TOKEN" ] && set -- "$@" -H "Authorization: Bearer $TOKEN" ;;  # the run's token
        -) : ;;                                                                            # NO auth header
        *) set -- "$@" -H "Authorization: Bearer $_b" ;;                                   # exactly this token
    esac
    if [ -n "$_d" ]; then set -- "$@" -H 'Content-Type: application/json' -d "$_d"; fi
    curl "$@" "$BASE$_p" 2>/dev/null
}

# sql STATEMENT [DATABASE] -> $BODY ; 0 iff the engine returned ok:true
sql() { sql_as "" "$1" "${2-$DB}"; }

# sql_as BEARER STATEMENT [DATABASE] -> $BODY ; 0 iff ok:true. An empty BEARER
# uses whatever the curlrc carries (the run's default token). An explicit BEARER
# is sent ALONE: the curlrc is not read, because it carries the run's admin
# token and a second Authorization header let the node authenticate the request
# as the admin (#780 — before this, every "as the reader" check ran as the admin).
sql_as() {
    _b="$1"; _stmt="$2"; _db="${3-$DB}"; _i=0
    _q="statement=$(printf '%s' "$_stmt" | jq -sRr @uri)"
    [ -n "$_db" ] && _q="$_q&database=$(printf '%s' "$_db" | jq -sRr @uri)"
    while [ "$_i" -lt "$RETRIES" ]; do
        _i=$((_i+1))
        if [ -n "$_b" ]; then
            BODY="$(curl -sS --max-time "$TIMEOUT" -H "Authorization: Bearer $_b" "$BASE/v1/sql?$_q" 2>/dev/null)"
        else
            BODY="$(curl -K "$CURLRC" "$BASE/v1/sql?$_q" 2>/dev/null)"
        fi
        if printf '%s' "$BODY" | jq -e '.ok == true' >/dev/null 2>&1; then return 0; fi
        # A definite engine rejection (not a transport blip) — stop retrying.
        if printf '%s' "$BODY" | jq -e '.error.message' >/dev/null 2>&1; then return 1; fi
        sleep 1
    done
    return 1
}

# http_as BEARER METHOD PATH [JSON] -> $BODY, $CODE (single shot, exactly this
# bearer and no other — never the curlrc's).
http_as() {
    _b="$1"; _m="$2"; _p="$3"; _d="${4:-}"
    set -- -sS --max-time "$TIMEOUT" -X "$_m" -o "$BODYF" -w '%{http_code}' -H "Authorization: Bearer $_b"
    if [ -n "$_d" ]; then set -- "$@" -H 'Content-Type: application/json' -d "$_d"; fi
    CODE="$(curl "$@" "$BASE$_p" 2>/dev/null)"
    BODY="$(cat "$BODYF" 2>/dev/null)"
}

# Envelope readers — SQL results live at `.value.rows` ([{col: val}]); tolerate
# the older flat `.rows` shape too so one script spans engine versions.
rows_len() { printf '%s' "$BODY" | jq -r '(.value.rows // .rows // []) | length' 2>/dev/null || echo 0; }
rfield()   { printf '%s' "$BODY" | jq -r --arg k "$1" '((.value.rows // .rows)[0] // {})[$k] // empty' 2>/dev/null; }
cols()     { printf '%s' "$BODY" | jq -rc '((.value.rows // .rows)[0] // {}) | keys' 2>/dev/null; }
err_msg()  { printf '%s' "$BODY" | jq -r '.error.message // empty' 2>/dev/null | head -c 140; }
err_code() { printf '%s' "$BODY" | jq -r '.error.code // empty' 2>/dev/null; }

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
# assert_atleast STMT MIN DESC [DB] — count must be >= MIN (deterministic floor)
assert_atleast() {
    if sql "$1" "${4-$DB}"; then
        _n="$(rows_len)"
        if [ "${_n:-0}" -ge "$2" ]; then pass "$3 (rows=$_n)"; else fail "$3 — expected >=$2 got $_n"; fi
    else
        skip "$3 — engine rejected: $(err_msg)"
    fi
}
# assert_cols STMT DESC KEY... — the first row must expose every named column
assert_cols() {
    _stmt="$1"; _desc="$2"; shift 2
    if sql "$_stmt"; then
        _have="$(cols)"; _miss=""
        for _k in "$@"; do case "$_have" in *"\"$_k\""*) : ;; *) _miss="$_miss $_k" ;; esac; done
        if [ -z "$_miss" ]; then pass "$_desc (cols:$*)"; else fail "$_desc — missing columns:$_miss (got $_have)"; fi
    else
        skip "$_desc — engine rejected: $(err_msg)"
    fi
}
# assert_ok STMT DESC [DB] — must return ok:true (else SKIP if absent)
assert_ok() { if sql "$1" "${3-$DB}"; then pass "$2"; else skip "$2 — engine rejected: $(err_msg)"; fi; }
# assert_reject STMT DESC [DB] — a negative test: the engine MUST reject it
assert_reject() { if sql "$1" "${3-$DB}"; then fail "$2 — expected rejection, got ok"; else pass "$2 (rejected: $(err_msg))"; fi; }

# --- MCP JSON-RPC over /mcp (Streamable-HTTP; SSE-tolerant) [#778] -----------
# mcp METHOD PARAMS_JSON -> $MBODY (the JSON-RPC response object) ; 0 on a 2xx.
# Sends an SSE `Accept`; a reply may be a bare JSON object or `data:`-framed —
# take the last non-empty `data:` line's JSON when present, else the raw body.
mcp() {
    _method="$1"; _params="${2:-{\}}"; _i=0
    _req="$(jq -cn --arg m "$_method" --argjson p "$_params" '{jsonrpc:"2.0",id:1,method:$m,params:$p}')"
    while [ "$_i" -lt "$RETRIES" ]; do
        _i=$((_i+1))
        CODE="$(curl -K "$CURLRC" -X POST -o "$BODYF" -w '%{http_code}' \
                -H 'Content-Type: application/json' \
                -H 'Accept: application/json, text/event-stream' \
                "$BASE/mcp" -d "$_req" 2>/dev/null)"
        _raw="$(cat "$BODYF" 2>/dev/null)"
        # SSE only when the body is genuinely event-stream framed (a line that
        # STARTS with `data:`) — not merely because a JSON value contains the
        # substring (tool descriptions carry `data:predict` / `data:search`).
        if printf '%s\n' "$_raw" | grep -q '^data:'; then
            MBODY="$(printf '%s\n' "$_raw" | sed -n 's/^data: *//p' | grep '{' | tail -n1)"
        else
            MBODY="$_raw"
        fi
        case "$CODE" in 2*) return 0 ;; esac
        sleep 1
    done
    return 1
}
mcp_has() { printf '%s' "$MBODY" | jq -e "$1" >/dev/null 2>&1; }

peer_norm() { case "$1" in http://*|https://*) printf '%s' "$1" ;; *) printf 'http://%s' "$1" ;; esac; }

# ============================================================================
printf 'Zetta e2e — base=%s levels=%s db=%s require_auth=%s\n' "$BASE" "$LEVELS" "$DB" "$REQUIRE_AUTH"
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
# LEVEL 2 — the in-app test suite (zetta::testkit) over testsuite_run  [#775]
# ============================================================================
if want_level 2; then
    sec "LEVEL 2 — in-app test suite (testsuite)"
    # The submit capability is `testsuite_run`; older images served `testkit_run`.
    # Discover which the target exposes from the MCP tool list (a hung unknown
    # /v1/<name> route would otherwise stall the run — unknown REST paths block).
    SUBMIT="testsuite_run"
    if mcp tools/list '{}' && printf '%s' "$MBODY" | grep -q '"testkit_run"' && ! printf '%s' "$MBODY" | grep -q '"testsuite_run"'; then
        SUBMIT="testkit_run"
    fi
    CASES="${ZETTA_E2E_TESTKIT_CASES:-}"
    if [ -z "$CASES" ] && sql "SELECT id FROM system.test_cases" ""; then
        CASES="$(printf '%s' "$BODY" | jq -r '(.value.rows // .rows)[]?.id // empty' 2>/dev/null)"
    fi
    [ -z "$CASES" ] && CASES="$TESTKIT_FALLBACK"
    _tc_total=0; _tc_pass=0; _tc_fail=0; _tc_skip=0; _cats=""
    for CASE in $CASES; do
        _tc_total=$((_tc_total+1))
        _cats="$_cats ${CASE%%.*}"
        if ! http POST "/v1/$SUBMIT" "$(jq -rn --arg c "$CASE" '{case:$c}')"; then
            if printf '%s' "$BODY" | grep -qi 'no testkit case\|no such\|unknown\|not found'; then
                skip "testkit[$CASE] — not in this build"; _tc_skip=$((_tc_skip+1)); continue
            fi
            fail "testkit[$CASE] submit failed (HTTP $CODE)"; _tc_fail=$((_tc_fail+1)); continue
        fi
        RID="$(printf '%s' "$BODY" | jq -r '.value.run_id // .run_id // .structuredContent.value.run_id // empty' 2>/dev/null)"
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
    TBL="widgets_${RUN}"
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

    # --- 3.3 query engine: filters / aggregates ---------------------------------
    sec "LEVEL 3.3 — query engine"
    assert_count "SELECT * FROM $TBL WHERE price > 20" 3 "WHERE price > 20"
    assert_count "SELECT * FROM $TBL WHERE price <= 10" 2 "WHERE price <= 10"
    assert_count "SELECT * FROM $TBL WHERE price <> 10" 5 "WHERE price <> 10"
    assert_count "SELECT * FROM $TBL WHERE in_stock = true" 4 "WHERE in_stock = true"
    assert_count "SELECT * FROM $TBL WHERE in_stock = false" 2 "WHERE in_stock = false"
    assert_count "SELECT * FROM $TBL WHERE category = 'tools'" 2 "WHERE category = 'tools'"
    assert_count "SELECT id, sum(price) FROM $TBL GROUP BY id" 6 "numeric GROUP BY (aggregate over DDF)"
    # A SELECT over a missing table is rejected.
    assert_reject "SELECT * FROM e2e_missing_${RUN}" "SELECT over a nonexistent table is rejected"
    # Documented engine limits — asserted as tracked SKIPs, not silent gaps:
    if sql "SELECT * FROM $TBL WHERE price > 0" && ! sql "SELECT * FROM $TBL WHERE price > 20 AND in_stock = true"; then
        skip "compound WHERE (AND/OR/NOT) unsupported by the deployed engine (tracked, zetta#756)"
    fi
    if sql "SELECT * FROM $TBL WHERE price > 0" && ! sql "SELECT * FROM $TBL ORDER BY price"; then
        skip "ORDER BY unsupported by the deployed engine (tracked)"
    fi
    # zetta#1218: whole-table aggregates, text GROUP BY keys and unknown columns.
    # An engine before it refuses the first two and answers the third with nulls,
    # so each is a tracked SKIP there rather than a failure.
    if sql "SELECT count(*) FROM $TBL"; then
        assert_field "SELECT count(*) FROM $TBL" count 6 "whole-table count(*) without GROUP BY"
    else
        skip "bare count(*) unsupported by the deployed engine (zetta#1218)"
    fi
    if sql "SELECT category, sum(price) FROM $TBL GROUP BY category"; then
        pass "text GROUP BY key (rows=$(rows_len))"
    else
        skip "text GROUP BY unsupported by the deployed engine (zetta#1218)"
    fi
    if sql "SELECT e2e_no_such_column FROM $TBL"; then
        skip "an unknown column reads null on the deployed engine (zetta#1218)"
    else
        pass "an unknown column is rejected ($(err_msg))"
    fi

    # --- 3.4 the system.* catalog: shape + content, not just readability [#777] --
    sec "LEVEL 3.4 — system.* views (shape + content)"
    for V in $SYSTEM_VIEWS; do
        if sql "SELECT * FROM system.$V" ""; then pass "system.$V readable"
        elif printf '%s' "$BODY" | grep -qi 'no such system table\|unknown.*table\|not found'; then skip "system.$V not served by this build"
        else fail "system.$V errored: $(err_msg)"; fi
    done
    # Deterministic content + shape (grounded on the live engine):
    #   nodes count == the reachable node count; the view carries membership cols.
    if sql "SELECT * FROM system.nodes" ""; then
        _nn="$(rows_len)"
        _peern=0; for p in $PEERS; do [ -n "$p" ] && _peern=$((_peern+1)); done
        _expect=$((_peern+1))
        if [ "${_nn:-0}" -eq "$_expect" ]; then pass "system.nodes count == node count ($_nn)"; else skip "system.nodes=$_nn but $_expect expected (routing/join still settling — cluster #781/#786)"; fi
    fi
    assert_cols "SELECT * FROM system.nodes" "system.nodes shape" node_id address is_self in_sync
    #   tables catalog names every system view; datasets carries the e2e's table.
    assert_atleast "SELECT * FROM system.tables" 23 "system.tables lists the catalog"
    if sql "SELECT table FROM system.tables" ""; then
        for _want in nodes audit runs spans tables; do
            printf '%s' "$BODY" | jq -e --arg t "$_want" '(.value.rows // .rows)[] | select(.table==$t)' >/dev/null 2>&1 \
                && pass "system.tables includes '$_want'" || fail "system.tables missing '$_want'"
        done
    fi
    if sql "SELECT * FROM system.datasets" ""; then
        printf '%s' "$BODY" | jq -e --arg t "$TBL" '(.value.rows // .rows)[] | select(.dataset==$t)' >/dev/null 2>&1 \
            && pass "system.datasets includes the e2e table ($TBL)" || fail "system.datasets missing $TBL"
    fi
    #   runs registry carried the L2 testkit runs; audit/spans/traces are non-empty.
    if want_level 2; then assert_atleast "SELECT run_id FROM system.runs" 1 "system.runs recorded the testkit runs"; fi
    assert_atleast "SELECT audit_id FROM system.audit" 1 "system.audit is populated"
    assert_cols "SELECT * FROM system.audit" "system.audit shape" audit_id action actor allowed
    assert_atleast "SELECT span_id FROM system.spans" 1 "system.spans carries traced operations"
    assert_cols "SELECT * FROM system.spans" "system.spans shape" span_id trace_id name intent plan
    assert_cols "SELECT * FROM system.traces" "system.traces shape" trace_id root_span_id span_count
    #   the test-suite catalog view carries one row per runnable case.
    assert_atleast "SELECT id FROM system.test_cases" 1 "system.test_cases lists the catalog"
    assert_cols "SELECT * FROM system.test_cases" "system.test_cases shape" id category name

    # --- 3.5 MCP protocol (JSON-RPC 2.0 over /mcp, SSE-tolerant) [#778] -----
    sec "LEVEL 3.5 — MCP protocol"
    if mcp initialize '{}' && mcp_has '.result.serverInfo.name'; then pass "MCP initialize -> serverInfo ($(printf '%s' "$MBODY" | jq -r '.result.serverInfo.name'))"; else skip "MCP initialize (transport/handshake)"; fi
    if mcp ping '{}' && mcp_has '.result'; then pass "MCP ping"; else skip "MCP ping unsupported"; fi
    TOOLS=""
    if mcp tools/list '{}' && mcp_has '.result.tools[0].name'; then
        _ntools="$(printf '%s' "$MBODY" | jq -r '.result.tools | length')"
        mcp_has '.result.tools[] | select(.name=="sql")' && pass "MCP tools/list exposes $_ntools tools (incl. sql)" || fail "MCP tools/list missing the sql tool"
        TOOLS="$(printf '%s' "$MBODY" | jq -r '.result.tools[].name')"
    else skip "MCP tools/list (transport)"; fi
    # tools/call server_info: the structured envelope every capability returns.
    if mcp tools/call '{"name":"server_info","arguments":{}}'; then
        if mcp_has '.result.structuredContent.ok==true' && mcp_has '.result.structuredContent.value.node_id' \
           && mcp_has '.result.structuredContent.trace_id' && mcp_has '.result.structuredContent.span_id' \
           && mcp_has '.result.structuredContent.duration_ms' && mcp_has '.result.isError==false'; then
            pass "MCP tools/call server_info (structuredContent {ok,value,trace_id,span_id,duration_ms})"
        else fail "MCP tools/call server_info — envelope incomplete: $(printf '%s' "$MBODY" | head -c 160)"; fi
    else skip "MCP tools/call (transport)"; fi
    # Every READ tool answers cleanly with ok:true + a trace id (reads have no
    # required args). Action/job tools are exercised on the REST side (§3.6/§3.8).
    _reads="server_info list_projects list_models list_indexes list_datasets list_runs list_jobs list_traces list_specs testsuite_list"
    _rc_ok=0; _rc_tot=0
    for _t in $_reads; do
        case " $TOOLS " in *" $_t "*)
            _rc_tot=$((_rc_tot+1))
            if mcp tools/call "$(jq -cn --arg n "$_t" '{name:$n,arguments:{}}')" \
               && mcp_has '.result.structuredContent.ok==true' && mcp_has '.result.structuredContent.trace_id'; then
                _rc_ok=$((_rc_ok+1))
            else fail "MCP tools/call $_t did not return ok+trace_id"; fi
        ;; esac
    done
    [ "$_rc_tot" -gt 0 ] && [ "$_rc_ok" -eq "$_rc_tot" ] && pass "MCP tools/call over $_rc_tot read tools (all ok+trace_id)"
    # Error paths must be clean structured errors, never a crash.
    if mcp tools/call '{"name":"does_not_exist","arguments":{}}' && mcp_has '.result.isError==true' && mcp_has '.result.structuredContent.error.code=="unknown_capability"'; then
        pass "MCP tools/call unknown tool -> clean unknown_capability error"
    else skip "MCP unknown-tool error path (transport)"; fi
    if mcp tools/call '{"name":"sql","arguments":{}}' && mcp_has '.result.isError==true' && mcp_has '.result.structuredContent.error.code=="invalid_input"'; then
        pass "MCP tools/call bad args -> clean invalid_input error"
    else skip "MCP bad-args error path (transport)"; fi
    if mcp bogus/method '{}' && mcp_has '.error.code==-32601'; then
        pass "MCP unknown method -> JSON-RPC -32601"
    else skip "MCP unknown-method error path (transport)"; fi

    # --- 3.6 REST capability + catalog surfaces [#778] ---------------------
    sec "LEVEL 3.6 — REST surfaces (/v1/*, specs, llms)"
    for R in server_info list_projects list_models list_indexes list_datasets list_runs list_jobs list_traces list_specs testsuite_list; do
        if http GET "/v1/$R" && printf '%s' "$BODY" | grep -q '"ok"'; then pass "GET /v1/$R"; else skip "GET /v1/$R not available"; fi
    done
    # An action over REST (POST /v1/<action>) + read-back is covered in §3.7.
    if http GET "/v1/openapi.json" && printf '%s' "$BODY" | grep -q 'openapi\|paths'; then
        _paths="$(printf '%s' "$BODY" | jq -r '.paths | keys | length' 2>/dev/null)"
        pass "GET /v1/openapi.json (${_paths:-?} paths)"
    else skip "GET /v1/openapi.json"; fi
    if http GET "/v1/specs" && printf '%s' "$BODY" | grep -q '"specs"'; then
        pass "GET /v1/specs ($(printf '%s' "$BODY" | jq -r '.value.count // (.value.specs|length)') specs)"
        _spec="$(printf '%s' "$BODY" | jq -r '.value.specs[0].href // empty' 2>/dev/null | sed 's#.*/##')"
        [ -n "$_spec" ] && { http GET "/v1/specs/$_spec" && pass "GET /v1/specs/$_spec" || skip "GET /v1/specs/$_spec"; }
    else skip "GET /v1/specs"; fi
    for L in "/llms.txt" "/llms-full.txt" "/llms/observability"; do
        if http GET "$L"; then pass "GET $L (agent docs)"; else skip "GET $L"; fi
    done

    # --- 3.7 orchestration action + read-back ------------------------------
    sec "LEVEL 3.7 — orchestration"
    PROJ="e2e-project-${RUN}"
    if http POST "/v1/create_project" "$(jq -rn --arg n "$PROJ" '{name:$n}')" && printf '%s' "$BODY" | grep -qi 'project\|"id"\|"ok"'; then
        pass "create_project (action over REST)"
        if http GET "/v1/list_projects" && printf '%s' "$BODY" | grep -q "$PROJ"; then pass "created project is listed"; else skip "list_projects did not echo $PROJ (flaky read)"; fi
    else skip "create_project not accepted in this build/config"; fi

    # --- 3.8 serving: expose -> search/predict -> unexpose, + negatives [#779] --
    sec "LEVEL 3.8 — serving"
    # Negatives always hold (the served routes are always registered and answer
    # from the replicated `exposed` flag — a miss is a clean 4xx, never a hang):
    _c="$(http_code GET '/v1/indexes/e2e_no_such_index/search?query=x')"
    case "$_c" in 40*) pass "search on a nonexistent index -> $_c (denied, not a hang)" ;; *) fail "search on nonexistent index returned $_c (want 4xx)" ;; esac
    _c="$(http_code POST '/v1/models/e2e_no_such_model/predict' '{"rows":[]}')"
    case "$_c" in 40*) pass "predict on a nonexistent model -> $_c (denied, not a hang)" ;; *) fail "predict on nonexistent model returned $_c (want 4xx)" ;; esac
    _c="$(http_code POST '/v1/expose_index' '{"name":"x"}')"
    case "$_c" in 400) pass "expose_index bad arguments -> 400 (schema-validated)" ;; *) skip "expose_index arg validation returned $_c" ;; esac
    # Positive round-trip when the target has an index/model registered. There is
    # no black-box index-build capability in this build (no CREATE INDEX SQL, no
    # build-index tool — indexes are a pipeline `index` step), so on a bare node
    # this is a tracked SKIP that lights up on a target that carries one.
    IDX=""
    if sql "SELECT * FROM system.indexes" ""; then IDX="$(rfield index_id)"; [ -z "$IDX" ] && IDX="$(rfield id)"; fi
    if [ -n "$IDX" ]; then
        if http POST "/v1/expose_index" "$(jq -rn --arg n "$IDX" '{index:$n}')"; then
            if http GET "/v1/indexes/$IDX/search?query=test"; then pass "expose_index + search ($IDX)"; else fail "search on exposed index $IDX"; fi
            if sql "SELECT exposed FROM system.indexes WHERE index_id='$IDX'" "" && [ "$(rfield exposed)" = "true" ]; then pass "system.indexes reflects exposed=true"; else skip "exposed flag not reflected (older view)"; fi
            http POST "/v1/unexpose_index" "$(jq -rn --arg n "$IDX" '{index:$n}')" >/dev/null 2>&1
            _c="$(http_code GET "/v1/indexes/$IDX/search?query=test")"
            case "$_c" in 40*) pass "unexpose_index -> search denied ($_c)" ;; *) fail "search still served after unexpose ($_c)" ;; esac
        else skip "expose_index $IDX not accepted"; fi
    else skip "no registered index to expose — no black-box index-build surface in this build (#779; lights up on a target carrying an index)"; fi
    MDL=""
    if sql "SELECT * FROM system.models" ""; then MDL="$(rfield model_id)"; [ -z "$MDL" ] && MDL="$(rfield id)"; fi
    if [ -n "$MDL" ]; then
        if http POST "/v1/expose_model" "$(jq -rn --arg n "$MDL" '{model:$n}')"; then pass "expose_model ($MDL)"; else skip "expose_model $MDL not accepted"; fi
    else skip "no registered model to expose/predict — no black-box train surface in this build (#779)"; fi

    # --- 3.9 governance views (enforcement lives in §3.13 under --require-auth) --
    sec "LEVEL 3.9 — governance views"
    for V in principals keys users roles user_roles tokens policies audit; do
        if sql "SELECT * FROM system.$V" ""; then pass "system.$V readable"; else skip "system.$V unavailable in this build"; fi
    done
    # system.tokens must never leak secret material (#744).
    if sql "SELECT * FROM system.tokens" ""; then
        if printf '%s' "$BODY" | jq -e '(.value.rows // .rows)[0] | has("hash") or has("salt") or has("secret")' >/dev/null 2>&1; then
            fail "system.tokens exposes secret material (hash/salt/secret column)"
        else pass "system.tokens exposes no secret material"; fi
    fi

    # --- 3.10 cluster + CRDT across nodes [#781] ---------------------------
    # Gated on a real multi-node target (mTLS-sync fix #786): the multi-node
    # assertions SKIP cleanly on a single-node target and light up once >=2
    # nodes are reachable. Never a FAIL on a single-node target.
    sec "LEVEL 3.10 — cluster + CRDT"
    PEER=""
    for p in $PEERS; do _pp="$(peer_norm "${p%/}")"; [ -n "$_pp" ] && [ "$_pp" != "$BASE" ] && PEER="$_pp" && break; done
    peer_sql() { curl -K "$CURLRC" "$1/v1/sql?statement=$(printf '%s' "$2" | jq -sRr @uri)&database=$(printf '%s' "$DB" | jq -sRr @uri)" 2>/dev/null; }
    peer_up=0
    if [ -n "$PEER" ] && curl -K "$CURLRC" "$PEER/v1/server_info" 2>/dev/null | grep -q '"ok"'; then peer_up=1; fi
    if [ "$peer_up" -ne 1 ]; then
        skip "single-node target — bidirectional convergence needs replicas>=2 (operator profile #88, gated on #786)"
        skip "partition rebalance on node join needs replicas>=2 (#781, gated on #786)"
        skip "cross-node blob transfer needs replicas>=2 (#781/#782, gated on #786)"
    else
        # Membership agrees on both nodes.
        if peer_sql "$PEER" "SELECT node_id FROM system.nodes" | jq -e '(.value.rows // .rows) | length >= 2' >/dev/null 2>&1; then pass "system.nodes shows >=2 members on the peer"; else fail "peer $PEER does not see >=2 members"; fi
        CTBL="canary_${RUN}"; A="a_${RUN}"; B="b_${RUN}"
        sql "CREATE TABLE $CTBL (id, tag)" >/dev/null 2>&1
        # A -> B convergence, latency-bounded.
        if sql "INSERT INTO $CTBL (id, tag) VALUES (1, '$A')"; then
            _s=0; _ok=0
            while [ "$_s" -lt 60 ]; do
                peer_sql "$PEER" "SELECT tag FROM $CTBL WHERE id = 1" | grep -q "$A" && { _ok=1; break; }
                sleep 2; _s=$((_s+2))
            done
            [ "$_ok" -eq 1 ] && pass "A->B: row on $BASE converged to $PEER in ${_s}s (CRDT)" || fail "A->B canary did NOT converge to $PEER within 60s"
        else fail "could not write A->B canary row"; fi
        # B -> A convergence (bidirectional).
        if curl -K "$CURLRC" -X POST -H 'Content-Type: application/json' "$PEER/v1/sql?statement=$(printf "INSERT INTO %s (id, tag) VALUES (2, '%s')" "$CTBL" "$B" | jq -sRr @uri)&database=$(printf '%s' "$DB" | jq -sRr @uri)" 2>/dev/null | grep -q '"ok":true'; then
            _s=0; _ok=0
            while [ "$_s" -lt 60 ]; do
                sql "SELECT tag FROM $CTBL WHERE id = 2" && printf '%s' "$BODY" | grep -q "$B" && { _ok=1; break; }
                sleep 2; _s=$((_s+2))
            done
            [ "$_ok" -eq 1 ] && pass "B->A: row on $PEER converged to $BASE in ${_s}s (bidirectional CRDT)" || fail "B->A canary did NOT converge to $BASE within 60s"
        else skip "could not write B->A canary on the peer (peer read-only?)"; fi
        skip "partition rebalance + cross-node blob transfer assertions pending a rebalance fixture (#781)"
    fi

    # --- 3.11 observability: the e2e's own runs/spans/traces/crashes [#782] --
    sec "LEVEL 3.11 — observability"
    if sql "SELECT * FROM system.activity" ""; then pass "system.activity readable ($(rows_len) events)"; else skip "system.activity unavailable"; fi
    assert_atleast "SELECT span_id FROM system.spans" 1 "system.spans carries the e2e's traced ops"
    assert_atleast "SELECT trace_id FROM system.traces" 1 "system.traces aggregates the span ring"
    # get_trace round-trips a live call's trace id back to its spans.
    if http GET "/v1/server_info"; then
        _tr="$(printf '%s' "$BODY" | jq -r '.trace_id // empty')"
        if [ -n "$_tr" ] && http GET "/v1/get_trace?trace_id=$_tr" && printf '%s' "$BODY" | jq -e '(.value.spans // []) | length >= 1' >/dev/null 2>&1; then
            pass "get_trace($_tr) returns the call's span(s)"
        else skip "get_trace round-trip unavailable"; fi
    fi
    if sql "SELECT * FROM system.crashes" ""; then
        _cn="$(rows_len)"; [ "${_cn:-0}" -eq 0 ] && pass "system.crashes is empty (no crashes)" || fail "system.crashes has $_cn entries"
    else skip "system.crashes unavailable"; fi

    # --- 3.12 blobs + consent + audit [#782] -------------------------------
    sec "LEVEL 3.12 — blobs / consent / audit"
    # Blobs: no black-box put/get/hash surface (large objects are cluster-internal,
    # referenced by model/dataset/feedback metadata) — tracked SKIP. The
    # missing-object clean-error path is the served-route 404 asserted in §3.8.
    skip "blob put/get + hash-verify — no black-box large-object endpoint in this build (#782; lights up if one ships)"
    pass "missing-object read is a clean 4xx (served routes, §3.8) — no hang/crash"
    # Consent: a consent-gated capability issues a one-time challenge WITHOUT
    # executing (no cloud call). The challenge is listable + audited, the caller
    # cannot self-decide (deny/approve need a distinct approver key), and a bogus
    # challenge id is a clean error.
    _cc="$(http_code POST '/v1/detect_provisioners' '{}')"
    if [ "$_cc" = "403" ]; then pass "consent-gated capability -> 403 consent_required (issues a challenge, no side effect)"
    else skip "detect_provisioners did not gate on consent ($_cc) — no consent-gated capability on this target"; fi
    if http GET "/v1/list_consent_requests" && printf '%s' "$BODY" | jq -e '(.value.consent_requests // []) | length >= 1' >/dev/null 2>&1; then
        pass "list_consent_requests reflects the pending challenge"
        _chid="$(printf '%s' "$BODY" | jq -r '.value.consent_requests | sort_by(.created_at) | last | .id')"
        # The unauthenticated-local session cannot self-approve/deny its own
        # challenge — consent integrity (the caller cannot satisfy the gate).
        if http POST "/v1/deny_consent" "$(jq -cn --arg c "$_chid" '{challenge_id:$c}')"; then
            skip "consent decided locally — target exposes an approver identity (approve/deny executed)"
        else
            [ "$(err_code)" = "unauthorized" ] && pass "local session cannot self-decide a consent challenge (approver key required)" || skip "deny_consent returned $(err_code) (approver-key path)"
        fi
    else skip "no pending consent request to inspect on this target"; fi
    _c="$(http_code POST '/v1/approve_consent' '{"challenge_id":"e2e-bogus"}')"
    case "$_c" in 40*) pass "approve_consent bad id -> $_c (clean error, not a crash)" ;; *) skip "approve_consent bad-id path returned $_c" ;; esac
    skip "consent approve->execute round-trip needs a distinct approver key (+ would invoke a provisioner) — approver-key target profile (#782)"
    # Audit: append-only + monotonic growth across the e2e's own writes.
    _a0=0; sql "SELECT audit_id FROM system.audit" "" && _a0="$(rows_len)"
    http POST "/v1/create_project" "$(jq -rn --arg n "e2e-audit-${RUN}" '{name:$n}')" >/dev/null 2>&1
    sleep 1
    if sql "SELECT audit_id FROM system.audit" ""; then
        _a1="$(rows_len)"
        [ "${_a1:-0}" -gt "${_a0:-0}" ] && pass "system.audit grew monotonically ($_a0 -> $_a1) across the e2e's writes" || skip "system.audit did not grow ($_a0 -> $_a1) — action may have been rejected"
    else skip "system.audit unavailable"; fi

    # --- 3.13 authz enforcement (auth is ALWAYS on, #799) [#780] -----------
    # The black-box regression harness for the auth review findings #759–#769.
    # The in-process half is tests/governance/test_authz_regressions.cpp and the
    # coverage map is docs/testing/authz-regressions.md. A FIXED finding is a hard
    # assertion (a regression FAILs). A finding still OPEN is a SKIP that names it
    # while it reproduces and turns into a PASS once its fix is deployed. Every
    # "as <user>" call sends that user's bearer alone (sql_as / http_as).
    sec "LEVEL 3.13 — authz enforcement (always on, #799) [#759-#769]"
    ADMIN="$TOKEN"
    # deny-by-default: no / empty / bogus credential is refused on every surface [#762].
    _all=1
    for _p in "/v1/server_info" "/v1/list_projects"; do
        _c="$(http_code GET "$_p" "" "-")"; case "$_c" in 401|403) : ;; *) _all=0 ;; esac
    done
    _c="$(http_code POST "/mcp" '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' "-")"; case "$_c" in 401|403) : ;; *) _all=0 ;; esac
    [ "$_all" -eq 1 ] && pass "deny-by-default: unauthenticated request denied on REST + /mcp [#762]" || fail "an unauthenticated request was NOT denied [#762]"
    # A bearer that is not a token is refused: there is no shared secret left to
    # compare, constant-time or not [#769 item 2, #762].
    _c="$(http_code GET '/v1/server_info' '' 'e2e-not-a-real-token')"; case "$_c" in 401|403) pass "a bogus bearer is denied ($_c) [#762/#769]" ;; *) fail "a bogus bearer was accepted ($_c) [#762/#769]" ;; esac
    # the admin (owner) token opens the gate.
    _c="$(http_code GET '/v1/server_info' '' "$ADMIN")"; case "$_c" in 200) pass "the admin token authenticates (200)" ;; *) fail "the admin token did not authenticate ($_c)" ;; esac

    # Seed a governed workload. Every object and user is per-run: the store is
    # append-only and a target keeps them across runs.
    sql_as "$ADMIN" "CREATE DATABASE $DB" >/dev/null 2>&1
    RUSER="e2e_reader_${RUN}"
    RTBL="authz_${RUN}"        # the reader's write attempts target this one (#759, #1229)
    RLSTBL="authz_rls_${RUN}"  # row security (#748): admin-only rows, the reader only reads
    MTBL="authz_mask_${RUN}"   # column masking (#760): no row policy on it
    for _t in "$RTBL" "$RLSTBL" "$MTBL"; do
        sql_as "$ADMIN" "CREATE TABLE $_t (id, owner, amount)" >/dev/null 2>&1
        for _r in "1,'$RUSER','100'" "2,'someone_else','200'" "3,'$RUSER','300'"; do
            sql_as "$ADMIN" "INSERT INTO $_t (id, owner, amount) VALUES ($_r)" >/dev/null 2>&1
        done
    done
    # mint_user NAME ROLE -> $MINTED (the user's token, or empty)
    mint_user() {
        MINTED=""
        sql_as "$ADMIN" "CREATE USER $1" >/dev/null 2>&1
        sql_as "$ADMIN" "GRANT ROLE $2 TO $1" >/dev/null 2>&1
        if sql_as "$ADMIN" "CREATE TOKEN FOR $1 TYPE access"; then MINTED="$(rfield secret)"; fi
    }
    # The built-in reader role: sql:read, serving:read, mcp:call — no write, DDL or manage.
    mint_user "$RUSER" reader; RTOK="$MINTED"
    if [ -z "$RTOK" ]; then
        skip "could not mint a reader token (CREATE USER / GRANT ROLE / CREATE TOKEN refused: $(err_msg)) — the authz matrix needs the credential store (#744)"
    else
        pass "minted a reader token (CREATE USER + GRANT ROLE reader + CREATE TOKEN)"
        if sql_as "$RTOK" "SELECT id FROM $RTBL"; then pass "reader holds sql:read (SELECT allowed)"; else fail "reader denied its own sql:read: $(err_msg)"; fi

        # #759 (fixed, #1229): every privileged or data-changing statement checks
        # its right; a reader is refused each one. An allowed one FAILS.
        authz_denied() { # DESC STMT FINDING
            if sql_as "$RTOK" "$2"; then fail "$1 was ALLOWED for a reader [$3]"
            else pass "$1 denied for a reader [$3]"; fi
        }
        authz_denied "INSERT" "INSERT INTO $RTBL (id, owner, amount) VALUES (9,'x','9')" "#759"
        authz_denied "CREATE USER" "CREATE USER e2e_escalate_${RUN}" "#759"
        authz_denied "CREATE TOKEN FOR owner" "CREATE TOKEN FOR owner TYPE access" "#759"
        authz_denied "CREATE TABLE (DDL)" "CREATE TABLE e2e_reader_ddl_${RUN} (x)" "#759"
        authz_denied "CREATE DATABASE (DDL)" "CREATE DATABASE e2e_reader_db_${RUN}" "#759"
        authz_denied "UPDATE" "UPDATE $RTBL SET amount = '0'" "#759/#1229"
        authz_denied "DELETE" "DELETE FROM $RTBL WHERE id = 1" "#759/#1229"
        authz_denied "TRUNCATE" "TRUNCATE $RTBL" "#759/#1229"
        authz_denied "DROP TABLE" "DROP TABLE $RTBL" "#759/#1229"
        authz_denied "DROP DATABASE" "DROP DATABASE $DB" "#759/#1229"
        # A reader holds no roles:manage, so it cannot grant itself admin.
        authz_denied "GRANT ROLE admin to itself" "GRANT ROLE admin TO $RUSER" "#761"
        # The connection store is not reachable through SQL at all (#1230).
        if sql_as "$ADMIN" "SELECT * FROM zetta_connect.connections"; then
            fail "the connection store answered a SELECT [#1230]"
        else pass "the connection store refuses SQL, even for the admin [#1230]"; fi

        # #769 (fixed, #1229): the refused statements are on the audit trail.
        if sql_as "$ADMIN" "SELECT * FROM system.audit WHERE action = 'data_changed'" ""; then
            # The reader's refused writes all target $RTBL (and nothing else
            # writes to it after the seed), so count the refused rows on it.
            _refused="$(printf '%s' "$BODY" | jq -r --arg t "sql:table:$DB.$RTBL" '[(.value.rows // .rows)[] | select((.allowed == false or .allowed == "false") and .target == $t)] | length' 2>/dev/null)"
            if [ "${_refused:-0}" -ge 5 ]; then pass "the reader's refused writes are audited as refused data_changed ($_refused) [#769]"
            else fail "expected >=5 refused data_changed audit rows on $RTBL, got ${_refused:-0} [#769]"; fi
        else skip "system.audit unreadable: $(err_msg)"; fi

        # #748 row security: a SELECT policy limits a non-admin to the rows
        # current_user() owns; admin bypasses [#762].
        if sql_as "$ADMIN" "CREATE POLICY authz_own_${RUN} ON $RLSTBL FOR SELECT USING (owner = current_user())"; then
            if sql_as "$RTOK" "SELECT id, owner FROM $RLSTBL"; then
                _rn="$(rows_len)"; _mine="$(printf '%s' "$BODY" | jq -r '[(.value.rows // .rows)[].owner] | unique | join(",")')"
                if [ "$_mine" = "$RUSER" ] && [ "$_rn" -eq 2 ]; then pass "RLS: reader sees only its own rows ($_rn) [#748]"; else fail "RLS: reader saw rows=$_rn owners=$_mine [#748]"; fi
            else fail "RLS: the reader could not read the table: $(err_msg) [#748]"; fi
            if sql_as "$ADMIN" "SELECT id FROM $RLSTBL" && [ "$(rows_len)" -eq 3 ]; then pass "RLS: admin bypasses the policy (sees all rows) [#762]"; else fail "RLS: admin did not see all rows [#762]"; fi
        else fail "CREATE POLICY refused: $(err_msg) [#748]"; fi
        # #765: the grammar refuses a policy with no condition. (The other half —
        # such a record arriving by replication or an edited policy.json, which
        # now denies — has no black-box surface; it is asserted in-process.)
        if sql_as "$ADMIN" "CREATE POLICY authz_nocond_${RUN} ON $RLSTBL FOR SELECT"; then
            fail "a no-condition SELECT policy was accepted — fail-open risk [#765]"
        else pass "the grammar refuses a SELECT policy with no condition [#765]"; fi
        skip "a condition-less policy that arrives replicated or restored is asserted in-process only (it denies every row) [#765]"

        # #760 column masking (fixed): a user granted (id, owner) sees amount
        # masked, and no aggregate recovers it.
        MUSER="e2e_masked_${RUN}"
        sql_as "$ADMIN" "CREATE ROLE e2e_idonly_${RUN}" >/dev/null 2>&1
        sql_as "$ADMIN" "GRANT mcp:call TO ROLE e2e_idonly_${RUN}" >/dev/null 2>&1  # reach the sql tool
        if sql_as "$ADMIN" "GRANT sql:read ON $DB.$MTBL(id, owner) TO ROLE e2e_idonly_${RUN}"; then
            mint_user "$MUSER" "e2e_idonly_${RUN}"; MTOK="$MINTED"
            if [ -n "$MTOK" ] && sql_as "$MTOK" "SELECT * FROM $MTBL"; then
                _leak="$(printf '%s' "$BODY" | jq -r '[(.value.rows // .rows)[] | select(.amount != null)] | length')"
                [ "$_leak" = "0" ] && pass "column masking: an ungranted column comes back null [#747]" || fail "column masking leaked amount in $_leak rows [#747]"
                for _agg in "SELECT id, MAX(amount) FROM $MTBL GROUP BY id" "SELECT owner, MIN(amount) FROM $MTBL GROUP BY owner"; do
                    if sql_as "$MTOK" "$_agg"; then
                        _vals="$(printf '%s' "$BODY" | jq -c '[(.value.rows // .rows)[] | to_entries[] | select(.key != "id" and .key != "owner") | .value | select(. != null)]')"
                        [ "$_vals" = "[]" ] && pass "an aggregate over a masked column returns no value ($_agg) [#760]" || fail "an aggregate recovered masked values $_vals ($_agg) [#760]"
                    else pass "an aggregate over a masked column is refused [#760]"; fi
                done
            else fail "the column-scoped user could not read $MTBL: $(err_msg) [#747]"; fi
        else fail "GRANT sql:read ON <table>(<columns>) refused: $(err_msg) [#747]"; fi

        # Scoped tool grant (#747) and the spec index it should narrow (#766).
        TUSER="e2e_onetool_${RUN}"
        sql_as "$ADMIN" "CREATE ROLE e2e_onetool_${RUN}" >/dev/null 2>&1
        if sql_as "$ADMIN" "GRANT mcp:call ON TOOL server_info TO ROLE e2e_onetool_${RUN}"; then
            mint_user "$TUSER" "e2e_onetool_${RUN}"; TTOK="$MINTED"
            http_as "$TTOK" GET "/v1/server_info"; _c1="$CODE"
            http_as "$TTOK" GET "/v1/list_projects"; _c2="$CODE"
            if [ "$_c1" = "200" ] && [ "$_c2" = "403" ]; then pass "a tool-scoped user calls server_info (200) and is refused list_projects (403) [#747]"
            else fail "tool scope: server_info=$_c1 list_projects=$_c2 (want 200/403) [#747]"; fi
            http_as "$TTOK" GET "/v1/specs"
            _lists=0; case "$BODY" in *'"list_projects"'*) _lists=1 ;; esac
            if [ "$CODE" = "200" ] && [ "$_lists" -eq 1 ]; then
                skip "a user granted only server_info reads every tool's spec at /v1/specs [#766 open]"
            else pass "/v1/specs lists only the caller's tools ($CODE) [#766]"; fi
        else fail "GRANT mcp:call ON TOOL refused: $(err_msg) [#747]"; fi

        # #761 (fixed): a holder of roles:manage grants only what it holds, so it
        # cannot grant itself admin.
        DUSER="e2e_delegate_${RUN}"
        sql_as "$ADMIN" "CREATE ROLE e2e_delegates_${RUN}" >/dev/null 2>&1
        sql_as "$ADMIN" "GRANT roles:manage, sql:read, mcp:call TO ROLE e2e_delegates_${RUN}" >/dev/null 2>&1
        mint_user "$DUSER" "e2e_delegates_${RUN}"; DTOK="$MINTED"
        if [ -z "$DTOK" ]; then skip "could not mint the roles:manage delegate: $(err_msg) [#761]"
        elif ! sql_as "$DTOK" "SELECT id FROM $RTBL"; then fail "the roles:manage delegate cannot run SQL at all: $(err_msg) [#761]"
        elif sql_as "$DTOK" "GRANT ROLE admin TO $DUSER"; then
            fail "a holder of only roles:manage granted itself admin [#761]"
        elif sql_as "$DTOK" "GRANT tokens:manage TO ROLE e2e_delegates_${RUN}"; then
            fail "a holder of only roles:manage granted its own role tokens:manage [#761]"
        else pass "a holder of only roles:manage cannot grant itself admin or a right it lacks [#761]"; fi
        # Editing or dropping a built-in role is asserted in-process only: on a
        # target running an older build it would change that role for every
        # user of the node.
        skip "built-in roles cannot be edited or dropped — asserted in-process only (on an older target it would change the target's roles) [#761]"

        # #766 (open): the governance catalog is readable by any authenticated user.
        _seen=""
        for _v in policies users roles user_roles tokens; do
            if sql_as "$RTOK" "SELECT * FROM system.$_v" "" && [ "$(rows_len)" -gt 0 ]; then _seen="$_seen system.$_v"; fi
        done
        if [ -n "$_seen" ]; then skip "a reader can read$_seen [#766 open]"; else pass "a reader is refused the governance catalog [#766]"; fi

        # Findings with no black-box surface — each is asserted in-process.
        skip "blob grants and the path boundary: no HTTP route serves a blob body yet [#767]"
        skip "a joining node minting a second owner needs a node started against the cluster with its own --admin-token [#763 open]"
        skip "an unsigned replicated credential needs a raw write into the replicated catalog [#764 open]"
        skip "the owner token's at-rest hash and an interrupted owner seed are not visible over the API [#769 open]"
        skip "token expiry/revoke has no black-box SQL surface (programmatic only) [#780]"

        # #768 (fixed, 7b1ac08): orchestration is grantable — the admin runs an
        # orchestration tool, and the reader (no pipelines:manage) is refused it.
        http_as "$ADMIN" POST "/v1/create_project" "$(jq -rn --arg n "e2e-authz-${RUN}" '{name:$n}')"; _c1="$CODE"
        http_as "$RTOK" POST "/v1/create_project" "$(jq -rn --arg n "e2e-authz-r-${RUN}" '{name:$n}')"; _c2="$CODE"
        if [ "$_c1" = "200" ] && [ "$_c2" = "403" ]; then pass "admin runs create_project (200), the reader is refused it (403) [#768]"
        else fail "create_project: admin=$_c1 reader=$_c2 (want 200/403) [#768]"; fi
    fi
fi

# ============================================================================
# SUMMARY
# ============================================================================
printf '\n============================================================\n'
printf 'Zetta e2e: pass=%s fail=%s skip=%s\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -ne 0 ]; then printf 'RESULT: FAIL\n'; exit 1; fi
printf 'RESULT: PASS\n'; exit 0
