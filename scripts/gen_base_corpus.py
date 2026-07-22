#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["requests", "huggingface-hub", "jsonschema"]
# ///
"""Generate the base-agent RLVR training corpus from the solutions catalog (zetta#866).

Method: blender-rl's *grow by generation, gate by verification* (see the
rlvr-training-data skill). It consumes the STRUCTURED catalog (build_catalog.py projects
docs/plan into catalog.json, schema-valid) — not raw markdown — and per use case:
  1. grounds the real-world data shape with Perplexity (optional),
  2. generates N task "games" with a frontier model via OpenRouter, grounded in the use
     case's SOLUTION spec + the methodology toolbox, phrased in rotating STYLES (the same
     outcome asked many ways) and sometimes transforming a pasted CURRENT STATE,
  3. VALIDATES each task against scripts/schemas/task.schema.json,
  4. GATES it through a mirror of the native `admit_corpus` (reference passes, negatives
     each fail), dropping any that don't validate or discriminate,
  5. streams the `CorpusTask` JSONL to --out (Ctrl-C leaves a valid partial), and
  6. (optionally) pushes to a PRIVATE HuggingFace dataset.

`--self-test` is fully offline (format + gate + schema). All credentials + config are read
from the ENVIRONMENT (never hardcoded):
  OPENROUTER_API_KEY / OPENROUTER_MODEL   frontier generation (default anthropic/claude-sonnet-4.5)
  PERPLEXITY_API_KEY / PERPLEXITY_MODEL   grounding (optional; default sonar-pro)
  HF_TOKEN, HF_DATASET_REPO               the private dataset to push to (only with --push)
  ORAMA_PLAN_DIR                          the solutions catalog dir (docs/plan)

Streams each task to --out as it is admitted (Ctrl-C keeps a valid partial). `--append`
**auto-resumes**: it reads the existing file, attributes each task to its use case (by the
`uc:` tag, or a use-case-name keyword match for older untagged rows), and tops each use
case up to --per-use-case — so re-running after an interruption or a credit top-up never
duplicates. `OPENROUTER_MAX_TOKENS` (default 4000) caps output per call.

Usage (always via `uv run`; deps declared inline above, PEP 723):
  uv run gen_base_corpus.py --self-test
  uv run gen_base_corpus.py --out corpus.jsonl                       # 20 samples per use case
  uv run gen_base_corpus.py --out corpus.jsonl --append              # resume where it left off
  uv run gen_base_corpus.py --out corpus.jsonl --current-state UC-1=charles.n8n.json --append --push
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Iterator

HERE = os.path.dirname(os.path.abspath(__file__))
TASK_SCHEMA = os.path.join(HERE, "schemas", "task.schema.json")

# The six execution-graph step kinds the agent mutates (ir::StepKind).
TOOL_NAMES = ["connect", "clean", "model", "index", "serve", "act"]


def default_plan_dir() -> str:
    env = os.environ.get("ORAMA_PLAN_DIR")
    if env:
        return env
    return os.path.normpath(
        os.path.join(HERE, "..", "..", "solutions-designs", "docs", "plan"))


# ---------------------------------------------------------------------------
# The admission gate + the schema — a task must VALIDATE and DISCRIMINATE. Both mirror
# the native side (agent/corpus_io.hpp + admission_gate.cpp), which re-checks before
# training; these are the generation-time pre-filters so nothing bad reaches the dataset.
# ---------------------------------------------------------------------------


def _grade_passes(required: dict[str, Any], observed: dict[str, Any]) -> bool:
    """A grade PASSES when every required fact is satisfied (>=1 required)."""
    if not required:
        return False
    return all(observed.get(key) == value for key, value in required.items())


def gate_task(row: dict[str, Any]) -> tuple[bool, str]:
    """Return (admitted, reason). Mirrors the native admission gate."""
    required = row.get("task", {}).get("required", {})
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


_VALIDATOR: Any = None


def validate_task(row: dict[str, Any]) -> tuple[bool, str]:
    """Return (ok, reason) — the task must adhere to task.schema.json."""
    global _VALIDATOR
    if _VALIDATOR is None:
        import jsonschema  # lazy; PEP 723 dep

        with open(TASK_SCHEMA, encoding="utf-8") as handle:
            _VALIDATOR = jsonschema.Draft7Validator(json.load(handle))
    errors = sorted(_VALIDATOR.iter_errors(row), key=lambda e: list(e.path))
    return (True, "") if not errors else (False, f"schema: {errors[0].message}")


# ---------------------------------------------------------------------------
# The churn seed — matches native churn_solution_case() (curriculum.cpp). Sample #1.
# ---------------------------------------------------------------------------

CHURN_REQUIRED = {
    "connect": True, "leakage_free": True, "model_kind": "gbdt",
    "explain": "shap", "calibrated": True, "calibration_loop": True,
}


def churn_self_test_row() -> dict[str, Any]:
    return {
        "task": {
            "id": "uc1-churn",
            "description": (
                "Build a churn-risk pipeline over the account events + communications: "
                "connect the source, engineer leakage-free point-in-time features, train "
                "a gradient-boosted model with SHAP explanations, and serve calibrated "
                "risk scores with a score-vs-outcome calibration loop."),
            "initial_state": {"data.churned_events": True, "data.active_accounts": True},
            "tool_names": TOOL_NAMES,
            "required": dict(CHURN_REQUIRED),
        },
        "reference": dict(CHURN_REQUIRED),
        "negative_controls": [
            {**CHURN_REQUIRED, "model_kind": "llm", "explain": "none"},
            {**CHURN_REQUIRED, "leakage_free": False},
            {**CHURN_REQUIRED, "calibration_loop": False},
        ],
        "tier": 1, "difficulty": 0.45, "held_out": False,
        "tags": ["churn", "tabular", "gbdt", "uc1"],
    }


# ---------------------------------------------------------------------------
# The structured catalog (from build_catalog.py) — the generator's grounding.
# ---------------------------------------------------------------------------


def load_catalog(plan_dir: str, catalog_path: str | None) -> dict[str, Any]:
    """Read a prebuilt catalog.json, or build it in-memory from the markdown plan."""
    if catalog_path:
        with open(catalog_path, encoding="utf-8") as handle:
            return json.load(handle)
    sys.path.insert(0, HERE)
    import build_catalog  # sibling script; build_catalog() needs no extra deps

    return build_catalog.build_catalog(plan_dir)


def methodology_toolbox(catalog: dict[str, Any]) -> str:
    return "\n".join(
        f"- {component['id']} — {component['name']}"
        for component in catalog.get("methodologies", {}).get("components", []))


def parse_current_states(pairs: list[str] | None) -> dict[str, dict[str, str]]:
    """`UC-N=path` -> {kind, text}. Kind inferred from the file extension."""
    kinds = {".json": "n8n", ".sql": "sql", ".ipynb": "notebook", ".py": "notebook"}
    states: dict[str, dict[str, str]] = {}
    for pair in pairs or []:
        uc, _, path = pair.partition("=")
        if not path:
            raise SystemExit(f"--current-state expects UC-N=path, got {pair!r}")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        states[uc.strip()] = {"kind": kinds.get(os.path.splitext(path)[1].lower(), "prose"),
                              "text": text}
    return states


# ---------------------------------------------------------------------------
# Grounding + generation (lazy imports; only touched when actually generating).
# ---------------------------------------------------------------------------

# The same outcome gets requested many ways; rotate the phrasing across a use case's
# samples so the agent learns intent invariance (varied ask -> same required end-state).
STYLES = [
    "a terse one-line request",
    "a detailed technical spec with explicit requirements",
    "a non-technical business ask, no ML jargon",
    "a vague, under-specified ask that still implies the goal",
    "a request that PASTES the user's current approach (an n8n workflow, a SQL script, a "
    "notebook, or a prose description of their manual/LLM-only process) and asks to improve it",
]

GENERATION_SYSTEM = """You author ONE verifiable RLVR training task for an agent that \
builds data/AI pipelines as an execution graph. Node kinds are exactly: connect, clean, \
model, index, serve, act.

blender-rl rule: the task must DISCRIMINATE. The `required` facts are the checkable \
success criterion (objectively verifiable from the built graph: model_kind, leakage_free, \
calibrated, ...), INDEPENDENT of how the request is phrased or what current state is pasted.
- `reference` = your `required` facts (a correct solution satisfies EXACTLY the criterion;
  use the SAME keys and values).
- Provide >=3 `negative_controls`. Each is the `required` facts copied, then AT LEAST ONE
  fact CHANGED to a WRONG value so it fails (the naive/LLM-only approach, a leaky pipeline,
  a missing-step pipeline). Same keys as `required`, one or more values wrong.

Real users ask for the same outcome in many different ways, and often PASTE their current \
state (an n8n workflow, a SQL script, a notebook, or a prose description of their \
manual/LLM-only process) and ask to improve it. Reflect that:
- Phrase `task.description` in the requested STYLE; keep the SAME required end-state.
- If a CURRENT STATE is provided, embed it in `task.description`, set \
`initial_state.current_state` (verbatim) + `initial_state.current_state_kind`, and frame \
the task as transforming it into the correct solution.
- Ground the required facts + reference in the provided SOLUTION SPEC and map them onto \
the METHODOLOGY TOOLBOX components.

Return ONLY a JSON object:
{ "task": {"id","description","initial_state":{...},"tool_names":["connect","clean",
    "model","index","serve","act"],"required":{...}},
  "reference": {...},                      // satisfies EVERY required fact
  "negative_controls": [{...}, ...],       // each fails >=1 required fact
  "tier": <0-5>, "difficulty": <0-1>, "held_out": false, "tags": [...] }"""


def perplexity_ground(use_case: dict[str, Any]) -> str:
    """Ground the data shape with Perplexity (sonar-pro). Optional + non-fatal."""
    key = os.environ.get("PERPLEXITY_API_KEY")
    if not key:
        return ""
    model = os.environ.get("PERPLEXITY_MODEL", "sonar-pro")
    import requests  # lazy

    prompt = (
        f"For the enterprise AI use case '{use_case['name']}', briefly describe the "
        "typical input data shape (tables + key columns), the standard modeling "
        "methodology, and the common leakage/calibration pitfalls. 6 sentences max.")
    try:
        resp = requests.post(
            "https://api.perplexity.ai/chat/completions",
            headers={"Authorization": f"Bearer {key}"},
            json={"model": model, "messages": [{"role": "user", "content": prompt}]},
            timeout=60)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]
    except Exception as exc:  # noqa: BLE001 - grounding is optional; degrade, don't crash
        print(f"  {use_case['id']}: grounding skipped ({exc})", file=sys.stderr)
        return ""


def _extract_json(content: Any, model: str) -> dict[str, Any]:
    """Pull the JSON object out of a model reply — tolerating ```json fences, prose, or a
    content-block list — and fail with a useful reason otherwise."""
    if isinstance(content, list):
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


def openrouter_generate(use_case: dict[str, Any], grounding: str, solution: str,
                        toolbox: str, style: str,
                        current_state: dict[str, str] | None) -> dict[str, Any]:
    key = os.environ["OPENROUTER_API_KEY"]  # required for generation
    model = os.environ.get("OPENROUTER_MODEL", "anthropic/claude-sonnet-4.5")
    import requests  # lazy

    parts = [
        f"Use case: {use_case['id']} — {use_case['name']} (band {use_case.get('band','?')}).",
        f"Phrasing style: {style}.",
    ]
    if current_state:
        parts.append(f"CURRENT STATE pasted by the user (kind: {current_state['kind']}) — "
                     f"the task is to transform THIS into the correct solution:\n"
                     f"{current_state['text'][:6000]}")
    if solution:
        parts.append("Our solution spec from the catalog (mirror its methodology "
                     f"composition, correctness checks, and agent operation):\n{solution[:3000]}")
    if toolbox:
        parts.append(f"Methodology toolbox (map required facts onto these):\n{toolbox}")
    if grounding:
        parts.append(f"External grounding:\n{grounding}")
    parts.append("Author one discriminating task in the given style. Output ONLY the JSON object.")

    resp = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={"Authorization": f"Bearer {key}"},
        json={"model": model, "temperature": 0.7,
              "max_tokens": int(os.environ.get("OPENROUTER_MAX_TOKENS", "4000")),
              "messages": [
                  {"role": "system", "content": GENERATION_SYSTEM},
                  {"role": "user", "content": "\n\n".join(parts)}]},
        timeout=120)
    if resp.status_code >= 400:
        raise RuntimeError(f"OpenRouter {resp.status_code}: {resp.text[:300]}")
    choices = resp.json().get("choices") or []
    if not choices:
        raise RuntimeError(f"no choices from '{model}'")
    return _extract_json((choices[0].get("message") or {}).get("content"), model)


def _attribute(row: dict[str, Any], keywords: dict[str, list[str]]) -> str | None:
    """Which use case an existing row belongs to: the `uc:<id>` tag if present (exact),
    else a keyword match of the use-case name against the task's id/description/tags — so
    resume works on rows written before the tag existed."""
    for tag in row.get("tags") or []:
        if isinstance(tag, str) and tag.startswith("uc:"):
            return tag[3:]
    task = row.get("task") or {}
    text = " ".join([str(task.get("id", "")), str(task.get("description", ""))]
                    + [t for t in row.get("tags") or [] if isinstance(t, str)]).lower()
    best, best_hits = None, 0
    for uc_id, words in keywords.items():
        hits = sum(1 for word in words if word in text)
        if hits > best_hits:
            best, best_hits = uc_id, hits
    return best if best_hits >= 1 else None


def resume_counts(path: str, use_cases: list[dict[str, Any]]) -> dict[str, int]:
    """Count tasks per use case already in the file we're appending to, so generation
    tops each use case up toward --per-use-case instead of duplicating. Attribution is by
    the `uc:` tag, falling back to a use-case-name keyword match for untagged rows."""
    counts: dict[str, int] = {}
    if not os.path.exists(path):
        return counts
    keywords = {u["id"]: sorted({w for w in re.findall(r"[a-z]{4,}", u["name"].lower())})
                for u in use_cases}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            uc = _attribute(row, keywords)
            if uc:
                counts[uc] = counts.get(uc, 0) + 1
    return counts


def generate_corpus(catalog: dict[str, Any], per_use_case: int,
                    current_states: dict[str, dict[str, str]],
                    existing: dict[str, int] | None = None,
                    emit_seeds: bool = True) -> Iterator[dict[str, Any]]:
    """Yield validated + gated CorpusTask rows — churn seed first, then per use case. Each
    row is tagged `uc:<id>`. `existing` (from resume_counts) tops each use case up toward
    `per_use_case`; `emit_seeds=False` skips the seed when resuming. A generator, so the
    caller streams each task (Ctrl-C keeps a partial)."""
    existing = existing or {}
    if emit_seeds:
        yield churn_self_test_row()  # sample #1, no API
    solutions = {s["uc"]: s.get("text", "") for s in catalog.get("solutions", []) if "uc" in s}
    toolbox = methodology_toolbox(catalog)
    paste_index = len(STYLES) - 1
    for use_case in catalog.get("use_cases", []):
        uc = use_case["id"]
        target = per_use_case - existing.get(uc, 0)
        if target <= 0:
            print(f"  {uc}: already complete ({existing.get(uc, 0)}), skipping", file=sys.stderr)
            continue
        grounding = perplexity_ground(use_case)
        solution = solutions.get(uc, "")
        for i in range(target):
            style = STYLES[i % len(STYLES)]
            current = current_states.get(uc) if i % len(STYLES) == paste_index else None
            try:
                row = openrouter_generate(use_case, grounding, solution, toolbox, style, current)
            except Exception as exc:  # noqa: BLE001 - a bad generation is skipped, logged
                print(f"  {uc}: generation failed: {exc}", file=sys.stderr)
                continue
            # A correct solution satisfies exactly the criterion: force the reference to
            # include every required fact by construction, so gating turns on whether the
            # NEGATIVE CONTROLS discriminate — not on the model duplicating keys verbatim.
            required = (row.get("task") or {}).get("required")
            if isinstance(required, dict) and required:
                row["reference"] = {**(row.get("reference") or {}), **required}
            if not isinstance(row.get("tags"), list):
                row["tags"] = []
            if f"uc:{uc}" not in row["tags"]:
                row["tags"].append(f"uc:{uc}")  # provenance, so --resume can skip it
            ok, reason = validate_task(row)
            if not ok:
                print(f"  {uc}: DROPPED ({reason})", file=sys.stderr)
                continue
            admitted, reason = gate_task(row)
            if not admitted:
                print(f"  {uc}: DROPPED (gate): {reason}", file=sys.stderr)
                continue
            print(f"  {uc}: admitted ({style.split(',')[0]})", file=sys.stderr)
            yield row


def push_to_hf(jsonl_path: str) -> None:
    repo = os.environ["HF_DATASET_REPO"]
    from huggingface_hub import HfApi  # lazy

    api = HfApi(token=os.environ["HF_TOKEN"])
    api.create_repo(repo, repo_type="dataset", private=True, exist_ok=True)
    api.upload_file(path_or_fileobj=jsonl_path, path_in_repo="corpus.jsonl",
                    repo_id=repo, repo_type="dataset")
    print(f"pushed {jsonl_path} -> hf://datasets/{repo} (private)")


# ---------------------------------------------------------------------------


def self_test() -> int:
    row = churn_self_test_row()
    assert json.loads(json.dumps(row)) == row, "row must be JSON round-trippable"
    ok, reason = validate_task(row)
    assert ok, f"churn seed must be schema-valid: {reason}"
    admitted, reason = gate_task(row)
    assert admitted, f"churn seed must be admitted: {reason}"
    for i, control in enumerate(row["negative_controls"]):
        bad, _ = gate_task({**row, "reference": control})
        assert not bad, f"negative control {i} should NOT pass the grader"
    print("self-test OK: churn seed is schema-valid, admitted, and discriminates.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="offline check of the format + schema + gate")
    parser.add_argument("--out", help="output JSONL path")
    parser.add_argument("--plan-dir", default=default_plan_dir())
    parser.add_argument("--catalog", help="prebuilt catalog.json (else built from --plan-dir)")
    parser.add_argument("--per-use-case", type=int, default=20)
    parser.add_argument("--current-state", action="append", metavar="UC-N=path",
                        help="hydrate a real current-state artifact for a use case (repeatable)")
    parser.add_argument("--append", action="store_true",
                        help="append to --out and AUTO-RESUME — read what's already there "
                             "and top each use case up to --per-use-case (no duplication)")
    parser.add_argument("--push", action="store_true",
                        help="upload --out to the private HF_DATASET_REPO")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.out:
        parser.error("--out is required (or use --self-test)")

    catalog = load_catalog(args.plan_dir, args.catalog)
    current_states = parse_current_states(args.current_state)
    # Auto-resume: appending to a non-empty file reads it and continues where it left off.
    resuming = args.append and os.path.exists(args.out) and os.path.getsize(args.out) > 0
    existing = resume_counts(args.out, catalog.get("use_cases", [])) if resuming else {}
    if resuming:
        print(f"resume: {sum(existing.values())} tasks already in {args.out} across "
              f"{len(existing)} use cases — topping up to {args.per_use_case} each",
              file=sys.stderr)
    print(f"catalog: {len(catalog.get('use_cases', []))} use cases, "
          f"{len(catalog.get('solutions', []))} solutions; "
          f"{args.per_use_case} samples each", file=sys.stderr)

    count = 0
    mode = "a" if args.append else "w"
    try:
        with open(args.out, mode, encoding="utf-8") as handle:
            for row in generate_corpus(catalog, args.per_use_case, current_states,
                                       existing, emit_seeds=not resuming):
                handle.write(json.dumps(row) + "\n")
                handle.flush()  # durable per task, so Ctrl-C leaves a valid partial corpus
                count += 1
    except KeyboardInterrupt:
        print(f"\ninterrupted — {count} tasks {'appended to' if args.append else 'written to'} "
              f"{args.out} (valid partial JSONL)", file=sys.stderr)
        return 130
    print(f"{'appended' if args.append else 'wrote'} {count} validated+gated tasks -> {args.out}")
    if args.push:
        push_to_hf(args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
