#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Validate every SKILL.md in .claude/skills/ so a skill cannot ship unloadable.

A skill whose YAML frontmatter does not parse is silently absent: no error, no
warning, the slash command is simply "unknown". That happened to
`zetta-walkthrough` — its description contained an unquoted `mcp-journeys: that
one...`, and a bare `": "` inside an unquoted YAML scalar makes the mapping
ambiguous, so the whole frontmatter failed and the skill never registered.

Checks, in the order they bite:
  1. frontmatter opens on line 1 and closes
  2. it parses as YAML (hand-rolled, stdlib only — PyYAML is not a dependency)
  3. `name` matches the directory and the naming rules
  4. `description` exists and is within the 1024-char limit

Exit 0 = every skill will load. Stdlib only; run from the repo root.
"""

import re
import sys
from pathlib import Path

MAX_DESCRIPTION = 1024
NAME_RULE = re.compile(r"^[a-z0-9-]{1,64}$")


def parse_frontmatter(text: str, problems: list[str]) -> dict[str, str] | None:
    """Parse the `---` block as a flat scalar mapping, the way a skill loader does.

    Deliberately strict: this is checking that the frontmatter is *unambiguous*,
    not being lenient about what a permissive parser might recover.
    """
    if not text.startswith("---\n"):
        problems.append("frontmatter must open with `---` on line 1")
        return None
    end = text.find("\n---", 3)
    if end == -1:
        problems.append("frontmatter is never closed with `---`")
        return None

    fields: dict[str, str] = {}
    for line_number, line in enumerate(text[4:end].split("\n"), start=2):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line[0] in " \t":
            problems.append(f"line {line_number}: indented — only flat keys are supported")
            continue
        key, separator, value = line.partition(":")
        if not separator:
            problems.append(f"line {line_number}: no `key: value`")
            continue
        value = value.strip()
        quoted = len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'"
        # The failure this script exists for: a bare `": "` inside an unquoted
        # scalar. YAML reads it as a nested mapping and the parse collapses.
        if not quoted and re.search(r":\s", value):
            problems.append(
                f"line {line_number}: `{key.strip()}` contains an unquoted ': ' — "
                "YAML reads that as a nested key and the skill will not load. "
                "Reword it, or wrap the whole value in double quotes."
            )
        fields[key.strip()] = value[1:-1] if quoted else value
    return fields


def check(skill: Path) -> list[str]:
    problems: list[str] = []
    md = skill / "SKILL.md"
    if not md.is_file():
        return [f"{skill.name}: no SKILL.md"]

    fields = parse_frontmatter(md.read_text(encoding="utf-8"), problems)
    if fields is None:
        return [f"{skill.name}: {p}" for p in problems]

    name = fields.get("name", "")
    if not name:
        problems.append("no `name`")
    elif not NAME_RULE.match(name):
        problems.append(f"`name: {name}` — lowercase letters, digits and '-' only, max 64")
    elif name != skill.name:
        problems.append(f"`name: {name}` does not match the directory `{skill.name}`")

    description = fields.get("description", "")
    if not description:
        problems.append("no `description` — without one the skill is never selected")
    elif len(description) > MAX_DESCRIPTION:
        problems.append(f"description is {len(description)} chars (max {MAX_DESCRIPTION})")

    return [f"{skill.name}: {p}" for p in problems]


def main() -> int:
    root = Path(".claude/skills")
    if not root.is_dir():
        print(f"no {root} here — run from the repo root", file=sys.stderr)
        return 2

    skills = sorted(d for d in root.iterdir() if d.is_dir())
    problems = [p for skill in skills for p in check(skill)]

    for problem in problems:
        print(f"  {problem}")
    print(f"{len(skills)} skills checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
