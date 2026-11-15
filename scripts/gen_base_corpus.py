#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

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

Beyond the (entirely supervised-tabular) catalog it also emits two built-in FAMILIES with
FIXED, discriminating targets — only the phrasing varies, so they work offline too:
  - SEARCH (--search-count) — upload a dataset, index it, serve ranked RETRIEVAL (keyword /
    semantic / hybrid); negatives are LLM-guessing, a LIKE scan, unranked results, and
  - ABSTAIN (--abstain-count) — an out-of-scope ask the agent must DECLINE, not fabricate a
    pipeline for; negatives are fabricated model/search solutions.
Without them a catalog-only agent borrows the churn template for search and hallucinates a
pipeline for "write me a poem" (observed in the trained 0.6B).

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
  uv run gen_base_corpus.py --out corpus.jsonl --current-state UC-1=current-workflow.n8n.json --append --push
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

Fit the required facts to THIS use case's real solution family — do NOT default to \
churn/binary. Choose `model_kind` from what the problem actually is: binary or multi-class \
classification, value/quantity regression, time-series or quantile forecasting, \
ranking/uplift, or clustering/segmentation. Name outcome/label facts after THIS problem (a \
non-churn task must NEVER carry a churn label), and include ONLY facts that apply (a \
forecasting task has temporal validation + a horizon, not calibration of a churn label).

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


def _openrouter_chat(system: str, user: str) -> dict[str, Any]:
    """One OpenRouter chat call, returning the reply's JSON object. Credentials + model come
    from the env (OPENROUTER_API_KEY / OPENROUTER_MODEL / OPENROUTER_MAX_TOKENS)."""
    key = os.environ["OPENROUTER_API_KEY"]  # required for any generation
    model = os.environ.get("OPENROUTER_MODEL", "anthropic/claude-sonnet-4.5")
    import requests  # lazy

    resp = requests.post(
        "https://openrouter.ai/api/v1/chat/completions",
        headers={"Authorization": f"Bearer {key}"},
        json={"model": model, "temperature": 0.7,
              "max_tokens": int(os.environ.get("OPENROUTER_MAX_TOKENS", "4000")),
              "messages": [{"role": "system", "content": system},
                           {"role": "user", "content": user}]},
        timeout=120)
    if resp.status_code >= 400:
        raise RuntimeError(f"OpenRouter {resp.status_code}: {resp.text[:300]}")
    choices = resp.json().get("choices") or []
    if not choices:
        raise RuntimeError(f"no choices from '{model}'")
    return _extract_json((choices[0].get("message") or {}).get("content"), model)


def openrouter_generate(use_case: dict[str, Any], grounding: str, solution: str,
                        toolbox: str, style: str,
                        current_state: dict[str, str] | None) -> dict[str, Any]:
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
    return _openrouter_chat(GENERATION_SYSTEM, "\n\n".join(parts))


# ---------------------------------------------------------------------------
# Built-in solution FAMILIES beyond the supervised-tabular catalog. Every catalog use case
# is supervised/tabular (churn, forecasting, fraud, ...), so a base agent trained on the
# catalog alone borrows that template for SEARCH and never learns to DECLINE an OUT-OF-SCOPE
# ask (it hallucinates a pipeline for "write me a poem"). These two families fix both. Their
# `required` facts are FIXED — a guaranteed-clean, discriminating target — and only the
# DESCRIPTION varies (frontier-phrased when OPENROUTER_API_KEY is set, the base intent
# verbatim otherwise, so a family is valid + gated even fully offline).
# ---------------------------------------------------------------------------

# --- SEARCH: upload a dataset, index it, serve retrieval (ir Index + Serve). It answers by
#     RETRIEVING ranked records from the corpus, never by an LLM guessing. Variants differ by
#     index kind: keyword (bm25f) / meaning (semantic) / both (hybrid). ---
_SEARCH_BASE = {
    "connect": True,           # upload / connect the dataset
    "index": True,             # build an index over it (ir::StepKind::Index)
    "retrieval": True,         # answer by retrieving matching records
    "ranked": True,            # relevance-ranked, not an unordered scan
    "serves_search": True,     # expose a search endpoint (expose_index)
    "grounded_in_data": True,  # results come from the corpus, never hallucinated
}
SEARCH_VARIANTS: dict[str, dict[str, Any]] = {
    "fulltext": {**_SEARCH_BASE, "index_kind": "bm25f"},
    "semantic": {**_SEARCH_BASE, "index_kind": "semantic", "embeddings": True},
    "hybrid": {**_SEARCH_BASE, "index_kind": "hybrid", "embeddings": True},
}
# (base intent, variant) — real "upload it and make it searchable" asks.
SEARCH_INTENTS: list[tuple[str, str]] = [
    ("We have 50,000 support articles — let people search them in plain English.", "semantic"),
    ("Upload our product catalog and let customers search it by keyword.", "fulltext"),
    ("Index our contracts so legal can find the right clause fast.", "hybrid"),
    ("Make our internal engineering wiki searchable.", "fulltext"),
    ("Load our past support tickets so agents can find similar resolved ones.", "semantic"),
    ("Our application logs are huge — give us fast full-text search over them.", "fulltext"),
    ("Ingest our FAQ and return the closest matching entries to a question.", "semantic"),
    ("Search across our PDFs and spreadsheets by meaning, not just exact words.", "hybrid"),
    ("Put our research library behind a search box.", "hybrid"),
    ("Let shoppers find products by describing what they want, not exact SKUs.", "semantic"),
]

SEARCH_DESCRIBE_SYSTEM = """You write ONLY the natural-language DESCRIPTION of a search/\
retrieval task for an agent that builds data pipelines (upload a dataset, index it, serve \
search). The correct solution is fixed elsewhere — do NOT output any solution facts. Given a \
base intent, a search kind, and a phrasing style, write a realistic request: a company with a \
concrete dataset (documents, tickets, a catalog, logs, contracts, ...) to upload and make \
searchable. For the 'paste current approach' style, embed what they do today (a SQL LIKE \
query, an Elasticsearch snippet, or 'we paste it into ChatGPT') and ask to improve it. Return \
ONLY JSON: {"description": "...", "initial_state": {"data.<name>": true}, "tags": ["..."]}"""


def _search_negatives(required: dict[str, Any]) -> list[dict[str, Any]]:
    """Plausible-wrong search solutions — each flips >= 1 required fact so it fails."""
    return [
        # ask an LLM instead of building an index (the anti-pattern) — no index, hallucinates
        {**required, "index": False, "index_kind": "llm", "retrieval": False,
         "grounded_in_data": False},
        # substring / SQL LIKE scan — no real index, results unranked
        {**required, "index": False, "index_kind": "substring", "ranked": False},
        # retrieves, but returns matches unranked (no relevance ordering)
        {**required, "ranked": False},
    ]


def _search_row(task_id: str, variant: str, description: str,
                initial_state: dict[str, Any], extra_tags: list[str]) -> dict[str, Any]:
    required = SEARCH_VARIANTS[variant]
    return {
        "task": {
            "id": task_id,
            "description": description,
            "initial_state": initial_state or {"data.corpus": True},
            "tool_names": ["connect", "clean", "index", "serve"],
            "required": dict(required),
        },
        "reference": dict(required),
        "negative_controls": _search_negatives(required),
        "tier": 2, "difficulty": 0.4, "held_out": False,
        "tags": ["search", "retrieval", variant, "uc:SEARCH", *extra_tags],
    }


def describe_search(intent: str, variant: str,
                    style: str) -> tuple[str, dict[str, Any], list[str]]:
    """Phrase a search intent (frontier model, optional). Falls back to the base intent
    verbatim with no API key — the fixed required facts already make the task valid + gated."""
    if not os.environ.get("OPENROUTER_API_KEY"):
        return intent, {"data.corpus": True}, []
    index_kind = SEARCH_VARIANTS[variant]["index_kind"]
    user = (f"Base intent: {intent}\nSearch kind: {variant} (index_kind={index_kind}).\n"
            f"Phrasing style: {style}.\nWrite the description.")
    try:
        out = _openrouter_chat(SEARCH_DESCRIBE_SYSTEM, user)
    except Exception as exc:  # noqa: BLE001 - phrasing is optional; degrade to the base intent
        print(f"  SEARCH: describe fell back ({exc})", file=sys.stderr)
        return intent, {"data.corpus": True}, []
    description = str(out.get("description") or "").strip() or intent
    initial_state = out.get("initial_state") if isinstance(out.get("initial_state"), dict) else {}
    tags = [t for t in (out.get("tags") or []) if isinstance(t, str)]
    return description, initial_state or {"data.corpus": True}, tags


def generate_search_corpus(target: int,
                           existing: dict[str, int] | None = None) -> Iterator[dict[str, Any]]:
    """Yield validated + gated SEARCH tasks (fixed retrieval facts, varied phrasing), cycling
    intents x styles. Resumes from the `uc:SEARCH` count in `existing`."""
    have = (existing or {}).get("SEARCH", 0)
    need = target - have
    if need <= 0:
        print(f"  SEARCH: already complete ({have}), skipping", file=sys.stderr)
        return
    combos = [(intent, variant, style)
              for (intent, variant) in SEARCH_INTENTS for style in STYLES]
    for i in range(need):
        intent, variant, style = combos[(have + i) % len(combos)]
        description, initial_state, extra_tags = describe_search(intent, variant, style)
        row = _search_row(f"search-{variant}-{have + i}", variant, description,
                          initial_state, extra_tags)
        ok, reason = validate_task(row)
        if not ok:
            print(f"  SEARCH: DROPPED ({reason})", file=sys.stderr)
            continue
        admitted, reason = gate_task(row)
        if not admitted:
            print(f"  SEARCH: DROPPED (gate): {reason}", file=sys.stderr)
            continue
        print(f"  SEARCH/{variant}: admitted ({style.split(',')[0]})", file=sys.stderr)
        yield row


# --- ABSTAIN: an OUT-OF-SCOPE request (not a data/AI-pipeline problem) the agent must
#     DECLINE rather than fabricate a pipeline. The correct end-state is the abstain marker;
#     every negative is a fabricated solution (it FAILED to decline). ---
ABSTAIN_REQUIRED = {"abstain": True, "in_scope": False}
ABSTAIN_NEGATIVES: list[dict[str, Any]] = [
    {"abstain": False, "in_scope": True, "model_kind": "gbdt", "calibrated": True},  # built a model
    {"abstain": False, "in_scope": True, "index_kind": "bm25f", "retrieval": True},  # built a search
    {"abstain": False, "in_scope": True},                                            # just answered it
]
ABSTAIN_INTENTS = [
    "Write me a poem about the ocean.",
    "What's the capital of France?",
    "Book me a flight to Tokyo next week.",
    "Tell me a joke.",
    "Translate 'good morning' into Japanese.",
    "What is 47 times 89?",
    "Summarize the plot of Hamlet.",
    "Recommend a good Italian restaurant near me.",
    "Draft a birthday message for my coworker.",
    "What's the weather going to be tomorrow?",
    "Explain how photosynthesis works.",
    "Set a reminder for my dentist appointment.",
]
ABSTAIN_MORE_SYSTEM = """You produce OUT-OF-SCOPE user messages for an agent that builds \
data/AI pipelines. Each message must be something the agent should politely DECLINE — NOT a \
data or modeling task: general knowledge, chit-chat, creative writing, math, translation, \
booking/errands, another tool's job. Do NOT mention datasets, models, predictions, search, \
or pipelines. Return ONLY JSON: {"messages": ["...", "..."]}"""


def _abstain_row(task_id: str, message: str) -> dict[str, Any]:
    return {
        "task": {
            "id": task_id,
            "description": message,
            "initial_state": {},
            "tool_names": TOOL_NAMES,
            "required": dict(ABSTAIN_REQUIRED),
        },
        "reference": dict(ABSTAIN_REQUIRED),
        "negative_controls": [dict(control) for control in ABSTAIN_NEGATIVES],
        "tier": 0, "difficulty": 0.2, "held_out": False,
        "tags": ["abstain", "out-of-scope", "uc:ABSTAIN"],
    }


def _more_abstain_intents(count: int) -> list[str]:
    """Optionally expand the out-of-scope pool with the frontier model (for diversity)."""
    if count <= 0 or not os.environ.get("OPENROUTER_API_KEY"):
        return []
    try:
        out = _openrouter_chat(ABSTAIN_MORE_SYSTEM,
                               f"Produce {count} varied out-of-scope messages.")
    except Exception as exc:  # noqa: BLE001 - expansion is optional
        print(f"  ABSTAIN: expansion skipped ({exc})", file=sys.stderr)
        return []
    return [m for m in (out.get("messages") or []) if isinstance(m, str) and m.strip()]


def generate_abstain_corpus(target: int,
                            existing: dict[str, int] | None = None) -> Iterator[dict[str, Any]]:
    """Yield validated + gated ABSTAIN tasks (out-of-scope -> decline). Resumes from the
    `uc:ABSTAIN` count; uses the built-in pool, optionally expanded by the frontier model."""
    have = (existing or {}).get("ABSTAIN", 0)
    need = target - have
    if need <= 0:
        print(f"  ABSTAIN: already complete ({have}), skipping", file=sys.stderr)
        return
    intents = list(ABSTAIN_INTENTS)
    if need > len(intents):
        intents += _more_abstain_intents(need - len(intents))
    for i in range(need):
        message = intents[(have + i) % len(intents)]
        row = _abstain_row(f"abstain-{have + i}", message)
        ok, reason = validate_task(row)
        if not ok:
            print(f"  ABSTAIN: DROPPED ({reason})", file=sys.stderr)
            continue
        admitted, reason = gate_task(row)
        if not admitted:
            print(f"  ABSTAIN: DROPPED (gate): {reason}", file=sys.stderr)
            continue
        print("  ABSTAIN: admitted", file=sys.stderr)
        yield row


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
    # SEARCH family — each variant is schema-valid, admitted, and discriminates.
    for variant in SEARCH_VARIANTS:
        search = _search_row(f"search-{variant}-0", variant, "Make our docs searchable.",
                             {"data.corpus": True}, [])
        ok, reason = validate_task(search)
        assert ok, f"search {variant} must be schema-valid: {reason}"
        admitted, reason = gate_task(search)
        assert admitted, f"search {variant} must be admitted: {reason}"
        for i, control in enumerate(search["negative_controls"]):
            assert not gate_task({**search, "reference": control})[0], \
                f"search {variant} negative {i} should NOT pass"
    # ABSTAIN family — an out-of-scope task declines; a fabricated solution fails.
    abstain = _abstain_row("abstain-0", "Write me a poem about the ocean.")
    ok, reason = validate_task(abstain)
    assert ok, f"abstain must be schema-valid: {reason}"
    admitted, reason = gate_task(abstain)
    assert admitted, f"abstain must be admitted: {reason}"
    for i, control in enumerate(abstain["negative_controls"]):
        assert not gate_task({**abstain, "reference": control})[0], \
            f"abstain negative {i} should NOT pass"
    print("self-test OK: churn + search (x3 variants) + abstain are schema-valid, admitted, "
          "and discriminate.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="offline check of the format + schema + gate")
    parser.add_argument("--out", help="output JSONL path")
    parser.add_argument("--plan-dir", default=default_plan_dir())
    parser.add_argument("--catalog", help="prebuilt catalog.json (else built from --plan-dir)")
    parser.add_argument("--per-use-case", type=int, default=20)
    parser.add_argument("--search-count", type=int, default=40,
                        help="SEARCH tasks (upload + index + serve retrieval); 0 to skip")
    parser.add_argument("--abstain-count", type=int, default=24,
                        help="ABSTAIN tasks (out-of-scope -> decline); 0 to skip")
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
          f"{args.per_use_case} samples each; families: search={args.search_count}, "
          f"abstain={args.abstain_count}", file=sys.stderr)

    def generate_all() -> Iterator[dict[str, Any]]:
        # supervised catalog first (churn seed included), then the built-in families.
        yield from generate_corpus(catalog, args.per_use_case, current_states,
                                   existing, emit_seeds=not resuming)
        if args.search_count > 0:
            yield from generate_search_corpus(args.search_count, existing)
        if args.abstain_count > 0:
            yield from generate_abstain_corpus(args.abstain_count, existing)

    count = 0
    mode = "a" if args.append else "w"
    try:
        with open(args.out, mode, encoding="utf-8") as handle:
            for row in generate_all():
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
