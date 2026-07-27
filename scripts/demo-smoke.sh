#!/usr/bin/env bash
# Drive the demo conversation against a running node and print what the agent actually says.
#
# This is the check that matters before showing anyone: the canary measures held-out cases WITH
# their retrieved context, but a demo opens before any Project exists, so its first turns are
# served ungrounded. A checkpoint can score 98% on the canary and still answer "set up a churn
# pipeline" with a governance notice — that happened — and only this script would have caught it.
#
# Usage:
#   scripts/demo-smoke.sh <agent-id> [base-url] [token]
#
# Exits non-zero if a beat fails its expectation, so it can gate a demo rather than just inform it.
set -uo pipefail

AGENT="${1:?usage: demo-smoke.sh <agent-id> [base-url] [token]}"
BASE="${2:-http://127.0.0.1:4800}"
TOKEN="${3:-${ZETTA_TOKEN:-zetta-local-dev}}"
TOKENS="${DEMO_MAX_TOKENS:-320}"

failures=0

# beat <expected-kind> <expect-pipeline: yes|no> <question>
beat() {
    local want_kind="$1" want_pipe="$2" question="$3"
    local body
    body=$(curl -s --max-time 600 -G -H "Authorization: Bearer ${TOKEN}" \
        --data-urlencode "agent=${AGENT}" \
        --data-urlencode "message=${question}" \
        --data-urlencode "max_new_tokens=${TOKENS}" \
        "${BASE}/v1/chat")

    printf '\n\033[1m── %s\033[0m\n' "${question}"
    KIND="${want_kind}" PIPE="${want_pipe}" python3 - "$body" <<'PY'
import json, os, sys
want_kind, want_pipe = os.environ["KIND"], os.environ["PIPE"] == "yes"
# Backticked things that are platform vocabulary rather than a claim about the user's workspace.
KNOWN_TERMS = {"python", "sql", "mcp", "rest", "treeshap", "bm25f", "/v1", "postgres",
               "snowflake", "kubernetes", "verification", "saved version", "pipeline"}
try:
    value = json.loads(sys.argv[1]).get("value", {})
except Exception:
    print("   FAIL: response was not JSON")
    raise SystemExit(2)

kind = value.get("kind")
steps = value.get("pipeline_steps")
reply = (value.get("reply") or "").strip()
pipeline = value.get("pipeline") or {}
nodes = pipeline.get("nodes") or []

print(f"   kind: {kind}   steps: {steps}")
if nodes:
    print("   pipe: " + " -> ".join(n["kind"] for n in nodes))
print("   " + (reply[:400] if reply else "(no reply)"))

problems = []
if kind != want_kind:
    problems.append(f"expected kind={want_kind}, got {kind}")
if want_pipe and not nodes:
    problems.append("expected a Pipeline, got none")
if not want_pipe and nodes:
    problems.append("expected NO Pipeline, got one")
if not reply:
    problems.append("empty reply")
# A raw envelope leaking to the user is a demo-ending failure on its own.
if reply.lstrip().startswith("{"):
    problems.append("raw JSON leaked into the user-facing reply")
# Naming a table or Project that does not exist. With nothing selected there is nothing
# legitimate to name, and the model was trained on a corpus where a table is ALWAYS in front of
# it — so it supplies one. This is how "With `customers` I can get to three things" passed a
# check on mode and Pipeline while being a fabrication, and how a migration answer came back
# offering to keep "`Orama Studio`" as the source.
import re
named = [m for m in re.findall(r"`([^`]+)`", reply) if m.lower() not in KNOWN_TERMS]
if named:
    problems.append("named entities that do not exist here: " + ", ".join(named))
# Node ids must be unique or the Pipeline will not compile.
ids = [n["id"] for n in nodes]
if len(set(ids)) != len(ids):
    problems.append("pipeline node ids collide")

for problem in problems:
    print(f"   \033[31mFAIL\033[0m: {problem}")
raise SystemExit(1 if problems else 0)
PY
    [ $? -ne 0 ] && failures=$((failures + 1))
}

echo "demo smoke — agent ${AGENT} at ${BASE}"

# The order a real conversation takes.
beat clarify no  "We have a bunch of customer and billing data and want to use AI. Where do we start?"
beat action  yes "Right now an n8n workflow calls an LLM on every row each night to score them. Can Orama do better?"
beat action  yes "Set up a churn prediction pipeline on our accounts data."
beat action  yes "Score our leads so sales works the ones most likely to convert."
beat action  yes "Predict which machines are likely to fail so we can service them first."
beat action  yes "Answer questions over our documents with cited passages, grounded in our data."
beat answer  no  "Why not just use an LLM for this?"
beat answer  no  "How do I know the score is right?"
beat answer  no  "Does my data leave my infrastructure?"
beat answer  no  "What languages can I write a step in?"
beat abstain no  "Write me a poem about the ocean."

printf '\n'
if [ "${failures}" -eq 0 ]; then
    printf '\033[32mall beats passed\033[0m\n'
else
    printf '\033[31m%s beat(s) failed\033[0m\n' "${failures}"
fi
exit $((failures > 0))
