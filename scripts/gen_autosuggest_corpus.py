#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["requests", "huggingface-hub", "jsonschema"]
# ///
"""Generate the Auto-Suggest training corpus (zetta#554, FEAT-11).

Auto-Suggest is a SEPARATE model from the pipeline-building agent (#555). It reads a
customer's CURRENT DATA STATE (their tables + columns) and suggests ZERO-to-many use
cases — each as a *prompt* that can be fed to the agent to produce an execution-graph
game. This generator produces its training data the blender-rl way (*grow by generation,
gate by verification*): the data state is constructed to support a KNOWN set of use cases,
so the correct suggestions are verifiable, and DISTRACTOR use cases (data absent) must NOT
be suggested — that is the discrimination signal.

It consumes the structured catalog (build_catalog.py) and, per use case, rotates
scenarios — single / multi / zero (garbage data supports nothing) — so the model learns
to suggest 0, 1, or many. Each sample is validated against
scripts/schemas/autosuggest.schema.json and gated (suggestions and distractors disjoint;
>= 1 distractor). Env-driven; `--self-test` is fully offline.

Env: OPENROUTER_API_KEY / OPENROUTER_MODEL, HF_TOKEN, HF_DATASET_REPO (the Auto-Suggest
dataset — separate from the agent corpus), ORAMA_PLAN_DIR.

`--append` **auto-resumes** (reads the file, tops each use case up to --per-use-case; no
duplication). `OPENROUTER_MAX_TOKENS` (default 4000) caps output per call.

Usage (via uv):
  uv run gen_autosuggest_corpus.py --self-test
  uv run gen_autosuggest_corpus.py --out autosuggest.jsonl        # 20 samples per use case
  uv run gen_autosuggest_corpus.py --out autosuggest.jsonl --append  # resume; --push to upload
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
from typing import Any, Iterator

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA = os.path.join(HERE, "schemas", "autosuggest.schema.json")


def default_plan_dir() -> str:
    env = os.environ.get("ORAMA_PLAN_DIR")
    if env:
        return env
    return os.path.normpath(
        os.path.join(HERE, "..", "..", "solutions-designs", "docs", "plan"))


def load_catalog(plan_dir: str, catalog_path: str | None) -> dict[str, Any]:
    if catalog_path:
        with open(catalog_path, encoding="utf-8") as handle:
            return json.load(handle)
    sys.path.insert(0, HERE)
    import build_catalog

    return build_catalog.build_catalog(plan_dir)


# ---------------------------------------------------------------------------
# Validation + the discrimination gate (mirrors the agent side's discipline).
# ---------------------------------------------------------------------------

_VALIDATOR: Any = None


def validate_sample(row: dict[str, Any]) -> tuple[bool, str]:
    global _VALIDATOR
    if _VALIDATOR is None:
        import jsonschema

        with open(SCHEMA, encoding="utf-8") as handle:
            _VALIDATOR = jsonschema.Draft7Validator(json.load(handle))
    errors = sorted(_VALIDATOR.iter_errors(row), key=lambda e: list(e.path))
    return (True, "") if not errors else (False, f"schema: {errors[0].message}")


def normalize_sample(row: dict[str, Any]) -> dict[str, Any]:
    """Coerce common model-formatting quirks into the schema shape: column entries -> name
    strings (models sometimes emit {name, type} objects), and use_case -> the bare id
    (models sometimes write 'UC-1 — Churn & retention')."""
    for table in (row.get("data_state") or {}).get("tables", []) or []:
        columns = table.get("columns")
        if isinstance(columns, list):
            table["columns"] = [
                (col.get("name") if isinstance(col, dict) else str(col))
                for col in columns
                if (col.get("name") if isinstance(col, dict) else col) is not None
            ]
    for key in ("suggestions", "distractors"):
        for item in row.get(key, []) or []:
            if isinstance(item, dict) and "use_case" in item:
                match = re.search(r"UC-\d+", str(item["use_case"]))
                if match:
                    item["use_case"] = match.group(0)
    return row


def gate_sample(row: dict[str, Any]) -> tuple[bool, str]:
    """Discrimination: a use case can't be both suggested and a distractor, and there must
    be >= 1 distractor (something the model must NOT suggest — else it could suggest all)."""
    suggested = {s["use_case"] for s in row.get("suggestions", [])}
    distractors = {d["use_case"] for d in row.get("distractors", [])}
    overlap = suggested & distractors
    if overlap:
        return False, f"{sorted(overlap)} is both suggested and a distractor"
    if not distractors:
        return False, "no distractors (nothing the model must NOT suggest — undiscriminating)"
    return True, ""


# ---------------------------------------------------------------------------
# Offline self-test samples (no API).
# ---------------------------------------------------------------------------


def churn_self_test_sample() -> dict[str, Any]:
    return {
        "data_state": {
            "description": "Account communications + churn history (Snowflake + Postgres).",
            "tables": [
                {"name": "account_registry", "columns": ["sfdc_account_id", "finance_segment"],
                 "profile": "~12k accounts, low null rate"},
                {"name": "account_events", "columns": ["event_date", "event_type", "comm_text"],
                 "profile": "~1.4M events, daily freshness"},
                {"name": "account_labels", "columns": ["churned", "churn_date", "last_churn_reason"],
                 "profile": "historical churn outcomes"},
            ],
        },
        "suggestions": [
            {"use_case": "UC-1", "name": "Churn & retention",
             "prompt": "Build a churn-risk model over the account events and communications: "
                       "leakage-free features, a gradient-boosted model with SHAP, calibrated "
                       "scores, and a score-vs-outcome calibration loop.",
             "rationale": "account_events + communications + account_labels are present"},
        ],
        "distractors": [
            {"use_case": "UC-5", "reason": "no time-series demand/quantity data present"},
            {"use_case": "UC-8", "reason": "no equipment sensor/telemetry data present"},
        ],
        "tier": 1, "difficulty": 0.4, "held_out": False, "tags": ["auto-suggest", "churn"],
    }


def zero_self_test_sample() -> dict[str, Any]:
    return {
        "data_state": {
            "description": "Only a generic marketing contact export — supports no modeled use case.",
            "tables": [{"name": "contacts", "columns": ["email", "first_name", "opt_in"],
                        "profile": "a flat contact list, no outcomes or events"}],
        },
        "suggestions": [],
        "distractors": [
            {"use_case": "UC-1", "reason": "no account events, communications, or churn labels"},
            {"use_case": "UC-4", "reason": "no invoices/receivables data"},
        ],
        "tier": 2, "difficulty": 0.5, "held_out": False, "tags": ["auto-suggest", "zero"],
    }


# ---------------------------------------------------------------------------
# Generation (lazy imports; only touched when actually generating).
# ---------------------------------------------------------------------------

GENERATION_SYSTEM = """You author ONE training sample for an Auto-Suggest model. That \
model reads a customer's CURRENT DATA STATE (tables + columns) and suggests the use cases \
the data SUPPORTS — each as a prompt to hand a pipeline-building agent — and must NOT \
suggest use cases the data cannot support.

You are given SUPPORTED use cases (their data must be PRESENT) and DISTRACTOR use cases \
(their data must be ABSENT). Produce:
- data_state.tables: realistic tables holding the data the SUPPORTED use cases need, PLUS \
1-3 irrelevant/noise tables, and NONE of the data a distractor needs. Each table's \
`columns` is an array of column-NAME STRINGS (e.g. "customer_id"), NOT objects.
- suggestions: one per SUPPORTED use case — {use_case, name, prompt (the intent to hand \
the agent), rationale (which tables/columns support it)}. If SUPPORTED is empty, [].
- distractors: the given DISTRACTOR use cases, each with a `reason` naming the missing data.
- Every `use_case` is the BARE id like "UC-1" — never "UC-1 — Churn & retention".

Return ONLY the JSON object:
{ "data_state": {"description", "tables":[{"name","columns":["col_a","col_b"],"profile"}]},
  "suggestions": [{"use_case":"UC-1","name","prompt","rationale"}],
  "distractors": [{"use_case":"UC-5","reason"}],
  "tier": <0-5>, "difficulty": <0-1>, "held_out": false, "tags": [...] }"""


def _extract_json(content: Any, model: str) -> dict[str, Any]:
    if isinstance(content, list):
        content = "".join(b.get("text", "") for b in content if isinstance(b, dict))
    text = (content or "").strip()
    if not text:
        raise RuntimeError(f"empty content from '{model}' (set OPENROUTER_MODEL)")
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


def openrouter_generate(supported: list[dict[str, str]], distractors: list[dict[str, str]],
                        anchor_solution: str) -> dict[str, Any]:
    key = os.environ["OPENROUTER_API_KEY"]
    model = os.environ.get("OPENROUTER_MODEL", "anthropic/claude-sonnet-4.5")
    import requests

    def label(use_cases: list[dict[str, str]]) -> str:
        return "; ".join(f"{u['id']} — {u['name']}" for u in use_cases) or "(none)"

    parts = [
        f"SUPPORTED use cases (their data must be present): {label(supported)}.",
        f"DISTRACTOR use cases (their data must be absent): {label(distractors)}.",
    ]
    if anchor_solution:
        parts.append("Grounding for the primary supported use case (its data shape / "
                     f"methodology):\n{anchor_solution[:2000]}")
    parts.append("Author one sample as specified. Output ONLY the JSON object.")

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
    """Which anchor use case a row belongs to: the `anchor:<id>` tag if present; else a
    single-suggestion sample's one suggestion; else a keyword match — so resume works on
    rows written before the tag existed."""
    for tag in row.get("tags") or []:
        if isinstance(tag, str) and tag.startswith("anchor:"):
            return tag[7:]
    suggestions = row.get("suggestions") or []
    if len(suggestions) == 1 and isinstance(suggestions[0], dict):
        match = re.search(r"UC-\d+", str(suggestions[0].get("use_case", "")))
        if match:
            return match.group(0)
    data_state = row.get("data_state") or {}
    text = " ".join([str(data_state.get("description", ""))]
                    + [str(s.get("name", "")) for s in suggestions if isinstance(s, dict)]
                    + [t for t in row.get("tags") or [] if isinstance(t, str)]).lower()
    best, best_hits = None, 0
    for uc_id, words in keywords.items():
        hits = sum(1 for word in words if word in text)
        if hits > best_hits:
            best, best_hits = uc_id, hits
    return best if best_hits >= 1 else None


def resume_counts(path: str, use_cases: list[dict[str, Any]]) -> dict[str, int]:
    """Count samples per anchor use case already in the file we're appending to (tag, or a
    fallback for untagged rows), so generation tops each use case up instead of duplicating."""
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


def generate_corpus(catalog: dict[str, Any], per_use_case: int, seed: int,
                    existing: dict[str, int] | None = None,
                    emit_seeds: bool = True) -> Iterator[dict[str, Any]]:
    """Yield validated + gated Auto-Suggest samples — churn + zero seeds first, then per use
    case rotating single / multi / zero scenarios. Each row is tagged `anchor:<id>`;
    `existing`/`emit_seeds` support auto-resume (see gen_base_corpus)."""
    existing = existing or {}
    if emit_seeds:
        yield churn_self_test_sample()
        yield zero_self_test_sample()
    rng = random.Random(seed)
    use_cases = [{"id": u["id"], "name": u["name"]} for u in catalog.get("use_cases", [])]
    solutions = {s["uc"]: s.get("text", "") for s in catalog.get("solutions", []) if "uc" in s}
    if not use_cases:
        return
    modes = ["single", "multi", "single", "multi", "zero"]  # ~20% zero-suggestion
    for anchor in use_cases:
        target = per_use_case - existing.get(anchor["id"], 0)
        if target <= 0:
            print(f"  {anchor['id']}: already complete ({existing.get(anchor['id'], 0)}), "
                  f"skipping", file=sys.stderr)
            continue
        others = [u for u in use_cases if u["id"] != anchor["id"]]
        for i in range(target):
            scenario = modes[i % len(modes)]
            pool = others[:]
            rng.shuffle(pool)
            if scenario == "zero":
                supported: list[dict[str, str]] = []
                distractors = [anchor] + pool[:rng.randint(1, 2)]
            elif scenario == "multi":
                supported = [anchor] + pool[:rng.randint(1, 2)]
                distractors = pool[3:3 + rng.randint(1, 2)] or pool[-1:]
            else:  # single
                supported = [anchor]
                distractors = pool[:rng.randint(1, 3)]
            try:
                row = openrouter_generate(supported, distractors, solutions.get(anchor["id"], ""))
            except Exception as exc:  # noqa: BLE001
                print(f"  {anchor['id']}/{scenario}: generation failed: {exc}", file=sys.stderr)
                continue
            row = normalize_sample(row)
            if not isinstance(row.get("tags"), list):
                row["tags"] = []
            if f"anchor:{anchor['id']}" not in row["tags"]:
                row["tags"].append(f"anchor:{anchor['id']}")  # provenance for --resume
            ok, reason = validate_sample(row)
            if not ok:
                print(f"  {anchor['id']}/{scenario}: DROPPED ({reason})", file=sys.stderr)
                continue
            admitted, reason = gate_sample(row)
            if not admitted:
                print(f"  {anchor['id']}/{scenario}: DROPPED (gate): {reason}", file=sys.stderr)
                continue
            print(f"  {anchor['id']}/{scenario}: admitted "
                  f"({len(row.get('suggestions', []))} suggestions)", file=sys.stderr)
            yield row


def push_to_hf(jsonl_path: str) -> None:
    repo = os.environ["HF_DATASET_REPO"]
    from huggingface_hub import HfApi

    api = HfApi(token=os.environ["HF_TOKEN"])
    api.create_repo(repo, repo_type="dataset", private=True, exist_ok=True)
    api.upload_file(path_or_fileobj=jsonl_path, path_in_repo="autosuggest.jsonl",
                    repo_id=repo, repo_type="dataset")
    print(f"pushed {jsonl_path} -> hf://datasets/{repo} (private)")


# ---------------------------------------------------------------------------


def self_test() -> int:
    for sample in (churn_self_test_sample(), zero_self_test_sample()):
        assert json.loads(json.dumps(sample)) == sample, "must be JSON round-trippable"
        ok, reason = validate_sample(sample)
        assert ok, f"sample must be schema-valid: {reason}"
        admitted, reason = gate_sample(sample)
        assert admitted, f"sample must be admitted: {reason}"
    # A sample that suggests everything (no distractors) must be REJECTED.
    bad = {**churn_self_test_sample(), "distractors": []}
    assert not gate_sample(bad)[0], "no-distractor sample must be rejected"
    print("self-test OK: churn + zero samples are schema-valid + discriminate; "
          "a no-distractor sample is rejected.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--out")
    parser.add_argument("--plan-dir", default=default_plan_dir())
    parser.add_argument("--catalog", help="prebuilt catalog.json (else built from --plan-dir)")
    parser.add_argument("--per-use-case", type=int, default=20)
    parser.add_argument("--seed", type=int, default=7)
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
    resuming = args.append and os.path.exists(args.out) and os.path.getsize(args.out) > 0
    existing = resume_counts(args.out, catalog.get("use_cases", [])) if resuming else {}
    if resuming:
        print(f"resume: {sum(existing.values())} samples already in {args.out} across "
              f"{len(existing)} use cases — topping up to {args.per_use_case} each",
              file=sys.stderr)
    print(f"catalog: {len(catalog.get('use_cases', []))} use cases; "
          f"{args.per_use_case} samples each", file=sys.stderr)
    count = 0
    mode = "a" if args.append else "w"
    try:
        with open(args.out, mode, encoding="utf-8") as handle:
            for row in generate_corpus(catalog, args.per_use_case, args.seed,
                                       existing, emit_seeds=not resuming):
                handle.write(json.dumps(row) + "\n")
                handle.flush()
                count += 1
    except KeyboardInterrupt:
        print(f"\ninterrupted — {count} samples written to {args.out} (valid partial)",
              file=sys.stderr)
        return 130
    print(f"{'appended' if args.append else 'wrote'} {count} validated+gated samples -> {args.out}")
    if args.push:
        push_to_hf(args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
