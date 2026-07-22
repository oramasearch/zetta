#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["requests", "huggingface-hub"]
# ///
"""Generate the base-agent RLVR training corpus from the solutions catalog (zetta#866).

Method: blender-rl's *grow by generation, gate by verification* (see the
rlvr-training-data skill). Per use case in `solutions-designs/docs/plan`, this:
  1. grounds the real-world data shape with Perplexity,
  2. generates N task "games" with a frontier model via OpenRouter — each a
     `Task` (intent + required end-state facts) + a reference solution that passes
     + negative controls that each fail (discrimination is the product),
  3. GATES every generated task through the same admission-gate logic the native
     `admit_corpus` enforces, dropping any that don't discriminate,
  4. writes JSONL in the `CorpusTask` shape the native loader consumes, and
  5. (optionally) pushes to a PRIVATE HuggingFace dataset.

Nothing external is contacted unless you run generation/push — `--self-test` is a
fully offline check of the format + gate logic (no deps, no network).

All credentials + config are read from the ENVIRONMENT (never hardcoded):
  OPENROUTER_API_KEY    frontier generation (Claude/GPT/... via OpenRouter)
  OPENROUTER_MODEL      default: anthropic/claude-sonnet-4.5
  PERPLEXITY_API_KEY    grounding the data shapes (optional; skipped if absent)
  HF_TOKEN, HF_DATASET_REPO   the private dataset to push to (only with --push)
  ORAMA_PLAN_DIR        the solutions catalog dir (default: the repo's sibling
                        solutions-designs/docs/plan)

Usage (always via `uv run`; deps are declared inline above, PEP 723):
  uv run gen_base_corpus.py --self-test                 # offline: format + gate
  uv run gen_base_corpus.py --out corpus.jsonl --per-use-case 3
  uv run gen_base_corpus.py --out corpus.jsonl --push   # + upload to HF_DATASET_REPO
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any

# The six execution-graph step kinds the agent mutates (ir::StepKind). A task's
# tools are these; a reference solution is a trajectory over them.
TOOL_NAMES = ["connect", "clean", "model", "index", "serve", "act"]

# ---------------------------------------------------------------------------
# The admission gate — a faithful mirror of the native `admit_corpus`
# (src/core/agent/admission_gate.cpp). The native loader re-gates before training;
# this is the generation-time pre-filter so ungated tasks never reach the dataset.
# ---------------------------------------------------------------------------


def _grade_passes(required: dict[str, Any], observed: dict[str, Any]) -> bool:
    """A grade PASSES when every required fact is satisfied (>=1 required)."""
    if not required:
        return False  # a task with no criterion is untrainable
    return all(observed.get(key) == value for key, value in required.items())


def gate_task(row: dict[str, Any]) -> tuple[bool, str]:
    """Return (admitted, reason). Mirrors the native admission gate."""
    task = row.get("task", {})
    required = task.get("required", {})
    if not isinstance(required, dict) or not required:
        return False, "untrainable task (no required facts)"
    if not _grade_passes(required, row.get("reference", {})):
        return False, "reference solution does not pass its grader"
    negatives = row.get("negative_controls", [])
    if not negatives:
        return False, "no negative controls (the gate would catch nothing)"
    for i, control in enumerate(negatives):
        if _grade_passes(required, control):
            return False, f"negative control {i} passed (no discrimination)"
    return True, ""


# ---------------------------------------------------------------------------
# The self-test corpus — the churn seed, matching the native `churn_solution_case()`
# (src/core/agent/curriculum.cpp). Proves the format + gate offline.
# ---------------------------------------------------------------------------

CHURN_REQUIRED = {
    "connect": True,
    "leakage_free": True,
    "model_kind": "gbdt",
    "explain": "shap",
    "calibrated": True,
    "calibration_loop": True,
}


def churn_self_test_row() -> dict[str, Any]:
    return {
        "task": {
            "id": "uc1-churn",
            "description": (
                "Build a churn-risk pipeline over the account events + communications: "
                "connect the source, engineer leakage-free point-in-time features, train "
                "a gradient-boosted model with SHAP explanations, and serve calibrated "
                "risk scores with a score-vs-outcome calibration loop."
            ),
            "initial_state": {"data.churned_events": True, "data.active_accounts": True},
            "tool_names": TOOL_NAMES,
            "required": dict(CHURN_REQUIRED),
        },
        "reference": dict(CHURN_REQUIRED),
        "negative_controls": [
            {**CHURN_REQUIRED, "model_kind": "llm", "explain": "none"},  # his n8n
            {**CHURN_REQUIRED, "leakage_free": False},  # leaky
            {**CHURN_REQUIRED, "calibration_loop": False},  # uncalibrated
        ],
        "tier": 1,
        "difficulty": 0.45,
        "held_out": False,
        "tags": ["churn", "tabular", "gbdt", "uc1"],
    }


# ---------------------------------------------------------------------------
# Catalog parsing — pull the scored use cases out of the plan (offline).
# ---------------------------------------------------------------------------


def default_plan_dir() -> str:
    env = os.environ.get("ORAMA_PLAN_DIR")
    if env:
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    # scripts/ -> zetta/ -> modules/ -> zetta-bridge/ -> modules/ -> solutions-designs
    return os.path.normpath(
        os.path.join(here, "..", "..", "solutions-designs", "docs", "plan")
    )


def parse_use_cases(plan_dir: str) -> list[dict[str, str]]:
    """Extract UC rows (id, name, band) from 02-use-case-portfolio.md's scored table."""
    path = os.path.join(plan_dir, "02-use-case-portfolio.md")
    use_cases: list[dict[str, str]] = []
    if not os.path.exists(path):
        return use_cases
    row = re.compile(r"^\|\s*(UC-\d+)\s*\|\s*([^|]+?)\s*\|.*\|\s*\*?\*?([ABC])\b")
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            match = row.match(line.strip())
            if match:
                use_cases.append(
                    {"id": match.group(1), "name": match.group(2).strip(),
                     "band": match.group(3)}
                )
    return use_cases


# ---------------------------------------------------------------------------
# Grounding + generation (lazy imports; only touched when actually generating).
# ---------------------------------------------------------------------------

GENERATION_SYSTEM = """You author ONE verifiable RLVR training task for an agent that \
builds data/AI pipelines as an execution graph. The graph's node kinds are exactly: \
connect, clean, model, index, serve, act. Follow blender-rl's rule: the task must \
DISCRIMINATE — a reference solution that passes and negative controls that each fail.

Return ONLY a JSON object with this shape (booleans/strings/numbers as facts):
{
  "task": {"id": "<slug>", "description": "<what to build>",
           "initial_state": {"<data facts available>": true},
           "tool_names": ["connect","clean","model","index","serve","act"],
           "required": {"<fact>": <value>, ...}},   // the checkable success criterion
  "reference": {"<fact>": <value>, ...},             // satisfies EVERY required fact
  "negative_controls": [{"<fact>": <value>, ...}, ...],  // each fails >=1 required fact
  "tier": <int 0-5>, "difficulty": <float 0-1>, "held_out": false, "tags": ["..."]
}
Required facts must be objectively checkable from the built graph (e.g. model_kind, \
leakage_free, calibrated). Provide at least 3 negative controls that are plausible-wrong \
(the naive/LLM-only approach, a leaky pipeline, a missing-step pipeline)."""


def perplexity_ground(use_case: dict[str, str]) -> str:
    key = os.environ.get("PERPLEXITY_API_KEY")
    if not key:
        return ""
    import requests  # lazy

    prompt = (
        f"For the enterprise AI use case '{use_case['name']}', briefly describe the "
        "typical input data shape (tables + key columns), the standard modeling "
        "methodology, and the common leakage/calibration pitfalls. 6 sentences max."
    )
    resp = requests.post(
        "https://api.perplexity.ai/chat/completions",
        headers={"Authorization": f"Bearer {key}"},
        json={"model": "sonar", "messages": [{"role": "user", "content": prompt}]},
        timeout=60,
    )
    resp.raise_for_status()
    return resp.json()["choices"][0]["message"]["content"]


def _extract_json(content: Any, model: str) -> dict[str, Any]:
    """Pull the JSON object out of a model reply — tolerating ```json fences, prose
    around it, or a content-block list — and fail with a useful reason otherwise."""
    if isinstance(content, list):  # some providers return a list of blocks
        content = "".join(b.get("text", "") for b in content if isinstance(b, dict))
    text = (content or "").strip()
    if not text:
        raise RuntimeError(
            f"empty content from '{model}' — verify the model id (set OPENROUTER_MODEL)")
    fence = re.search(r"```(?:json)?\s*(\{.*\})\s*```", text, re.DOTALL)
    if fence:
        text = fence.group(1)
    else:
        start, end = text.find("{"), text.rfind("}")
        if 0 <= start < end:
            text = text[start:end + 1]
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"non-JSON from '{model}': {content[:200]!r}") from exc


def openrouter_generate(use_case: dict[str, str], grounding: str) -> dict[str, Any]:
    key = os.environ["OPENROUTER_API_KEY"]  # required for generation
    model = os.environ.get("OPENROUTER_MODEL", "anthropic/claude-sonnet-4.5")
    import requests  # lazy

    user = (
        f"Use case: {use_case['id']} — {use_case['name']} (band {use_case['band']}).\n"
        f"Grounding:\n{grounding or '(none)'}\n\n"
        "Author one discriminating task as specified. Output ONLY the JSON object."
    )
    resp = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={"Authorization": f"Bearer {key}"},
        json={
            "model": model,
            "messages": [
                {"role": "system", "content": GENERATION_SYSTEM},
                {"role": "user", "content": user},
            ],
            "temperature": 0.7,
        },
        timeout=120,
    )
    if resp.status_code >= 400:  # surface model-not-found / auth errors verbatim
        raise RuntimeError(f"OpenRouter {resp.status_code}: {resp.text[:300]}")
    payload = resp.json()
    choices = payload.get("choices") or []
    if not choices:
        raise RuntimeError(f"no choices from '{model}': {json.dumps(payload)[:300]}")
    return _extract_json((choices[0].get("message") or {}).get("content"), model)


def generate_corpus(plan_dir: str, per_use_case: int) -> list[dict[str, Any]]:
    use_cases = parse_use_cases(plan_dir)
    if not use_cases:
        print(f"warning: no use cases found under {plan_dir}", file=sys.stderr)
    rows: list[dict[str, Any]] = [churn_self_test_row()]  # churn is always sample #1
    for use_case in use_cases:
        grounding = perplexity_ground(use_case)
        for _ in range(per_use_case):
            try:
                row = openrouter_generate(use_case, grounding)
            except Exception as exc:  # noqa: BLE001 - a bad generation is skipped, logged
                print(f"  {use_case['id']}: generation failed: {exc}", file=sys.stderr)
                continue
            admitted, reason = gate_task(row)
            if not admitted:
                print(f"  {use_case['id']}: DROPPED (gate): {reason}", file=sys.stderr)
                continue
            rows.append(row)
            print(f"  {use_case['id']}: admitted", file=sys.stderr)
    return rows


def push_to_hf(jsonl_path: str) -> None:
    repo = os.environ["HF_DATASET_REPO"]
    token = os.environ["HF_TOKEN"]
    from huggingface_hub import HfApi  # lazy

    api = HfApi(token=token)
    api.create_repo(repo, repo_type="dataset", private=True, exist_ok=True)
    api.upload_file(
        path_or_fileobj=jsonl_path,
        path_in_repo="corpus.jsonl",
        repo_id=repo,
        repo_type="dataset",
    )
    print(f"pushed {jsonl_path} -> hf://datasets/{repo} (private)")


# ---------------------------------------------------------------------------


def self_test() -> int:
    row = churn_self_test_row()
    line = json.dumps(row)
    assert json.loads(line) == row, "row must be JSON round-trippable"
    admitted, reason = gate_task(row)
    assert admitted, f"churn seed must be admitted, got: {reason}"
    # Each negative control must fail the gate when substituted as the reference.
    for i, control in enumerate(row["negative_controls"]):
        bad = {**row, "reference": control}
        ok, _ = gate_task(bad)
        assert not ok, f"negative control {i} should NOT pass the grader"
    print("self-test OK: churn seed round-trips, is admitted, and discriminates.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="offline check of the format + gate (no network/deps)")
    parser.add_argument("--out", help="output JSONL path")
    parser.add_argument("--plan-dir", default=default_plan_dir(),
                        help="solutions catalog dir (docs/plan)")
    parser.add_argument("--per-use-case", type=int, default=2)
    parser.add_argument("--push", action="store_true",
                        help="upload --out to the private HF_DATASET_REPO")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if not args.out:
        parser.error("--out is required (or use --self-test)")
    rows = generate_corpus(args.plan_dir, args.per_use_case)
    with open(args.out, "w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row) + "\n")
    print(f"wrote {len(rows)} gated tasks -> {args.out}")
    if args.push:
        push_to_hf(args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
