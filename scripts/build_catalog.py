#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

# /// script
# requires-python = ">=3.11"
# dependencies = ["jsonschema"]
# ///
"""Project solutions-designs/docs/plan (markdown) into a structured, schema-valid
catalog.json for the training-data generators to consume (zetta#866).

The plan is prose + tables; regex-scraping it inside the generator was fragile. This is
the one place that reads the markdown, and it emits a machine-readable catalog validated
against scripts/schemas/catalog.schema.json:
  - use_cases   : id, name, band, scores (from 02's scored table)
  - methodologies.components (C-n) / .methods (M-n)  (from 03 — the agent's tools)
  - solutions   : id, uc, name, + the S-n spec section text (from 04's 12-field anatomy)

The `text` fields carry the raw section markdown so the generator grounds each task in
the real spec. An optional --enrich pass (OpenRouter) can later structure the prose into
the 12 fields; v1 is fully programmatic + offline.

Usage (via uv):
  uv run build_catalog.py --self-test              # build from the real plan + validate
  uv run build_catalog.py --out catalog.json       # write the structured catalog
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG_SCHEMA = os.path.join(HERE, "schemas", "catalog.schema.json")


def default_plan_dir() -> str:
    env = os.environ.get("ORAMA_PLAN_DIR")
    if env:
        return env
    return os.path.normpath(
        os.path.join(HERE, "..", "..", "solutions-designs", "docs", "plan"))


def _read(plan_dir: str, name: str) -> str:
    path = os.path.join(plan_dir, name)
    return open(path, encoding="utf-8").read() if os.path.exists(path) else ""


def _sections(text: str, header_re: str) -> list[tuple[re.Match, str]]:
    """(header match, body-to-next-same-header) for each header in `text`."""
    matches = list(re.finditer(header_re, text, re.MULTILINE))
    out = []
    for i, match in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        out.append((match, text[match.start():end].strip()))
    return out


def _name_after_dash(header_line: str) -> str:
    """'### C-1 — Connectors / extraction' -> 'Connectors / extraction'."""
    parts = re.split(r"\s[—-]\s", header_line, maxsplit=1)
    return parts[1].strip() if len(parts) > 1 else header_line.strip()


def parse_use_cases(text: str) -> list[dict[str, Any]]:
    """From 02's 'scored portfolio at a glance' table:
    | UC | name | V·V·V·V | E[ROI]× | P·P·P·P·P | p | RA× | Band |"""
    use_cases: list[dict[str, Any]] = []
    seen: set[str] = set()
    for line in text.splitlines():
        if not line.strip().startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not cells or not re.match(r"^UC-\d+$", cells[0]) or cells[0] in seen:
            continue
        band = None
        for cell in reversed(cells):
            match = re.search(r"\b([ABC])\b", cell.replace("*", ""))
            if match:
                band = match.group(1)
                break
        scores: dict[str, Any] = {"raw": cells[2:]}
        if len(cells) >= 7:
            scores.update({"value_axes": cells[2], "e_roi": cells[3],
                           "feasibility_axes": cells[4], "p_success": cells[5],
                           "ra": cells[6]})
        seen.add(cells[0])
        use_cases.append({"id": cells[0], "name": cells[1], "band": band or "A",
                          "scores": scores})
    return use_cases


def parse_components(text: str) -> list[dict[str, Any]]:
    components = []
    for match, body in _sections(text, r"^### +C-\d+\b.*$"):
        cid = re.match(r"^### +(C-\d+)", match.group(0)).group(1)
        components.append({"id": cid, "name": _name_after_dash(match.group(0)),
                           "text": body})
    return components


def parse_methods(text: str) -> list[dict[str, Any]]:
    methods = []
    family = ""
    for line in text.splitlines():
        fam = re.match(r"^### +(Family .+)$", line)
        if fam:
            family = fam.group(1).strip()
            continue
        met = re.match(r"^#### +(M-\d+)\b(.*)$", line)
        if met:
            methods.append({"id": met.group(1), "name": _name_after_dash(line),
                            "family": family})
    return methods


def parse_solutions(text: str) -> list[dict[str, Any]]:
    solutions = []
    for match, body in _sections(text, r"^## +\d+\.\s+S-\d+\b.*$"):
        header = match.group(0)
        sid = re.search(r"(S-\d+)", header).group(1)
        entry: dict[str, Any] = {"id": sid, "name": _name_after_dash(header), "text": body}
        uc = re.search(r"\(UC-(\d+)\)", header)
        if uc:
            entry["uc"] = f"UC-{uc.group(1)}"
        solutions.append(entry)
    return solutions


def build_catalog(plan_dir: str) -> dict[str, Any]:
    uc_md, meth_md, sol_md = (_read(plan_dir, f) for f in (
        "02-use-case-portfolio.md", "03-methodology-portfolio.md",
        "04-solutions-catalog.md"))
    return {
        "source": {"plan_dir": plan_dir,
                   "files": ["02-use-case-portfolio.md", "03-methodology-portfolio.md",
                             "04-solutions-catalog.md"]},
        "use_cases": parse_use_cases(uc_md),
        "methodologies": {"components": parse_components(meth_md),
                          "methods": parse_methods(meth_md)},
        "solutions": parse_solutions(sol_md),
    }


def validate(catalog: dict[str, Any]) -> None:
    import jsonschema  # lazy; PEP 723 dep

    with open(CATALOG_SCHEMA, encoding="utf-8") as handle:
        schema = json.load(handle)
    jsonschema.validate(catalog, schema)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="build from the real plan + validate (offline)")
    parser.add_argument("--plan-dir", default=default_plan_dir())
    parser.add_argument("--out", help="write catalog.json here")
    args = parser.parse_args()

    catalog = build_catalog(args.plan_dir)
    validate(catalog)
    counts = (f"{len(catalog['use_cases'])} use cases, "
              f"{len(catalog['methodologies']['components'])} components, "
              f"{len(catalog['methodologies']['methods'])} methods, "
              f"{len(catalog['solutions'])} solutions")
    if args.self_test:
        print(f"self-test OK: schema-valid — {counts}")
        return 0
    if not args.out:
        parser.error("--out is required (or use --self-test)")
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(catalog, handle, indent=2, ensure_ascii=False)
    print(f"wrote {counts} -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
