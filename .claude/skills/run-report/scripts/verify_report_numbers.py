#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Check that every number in a run report traces back to the facts artifact.

    python3 verify_report_numbers.py <report.md> <run-report.json>

Exits non-zero and lists any figure that cannot be traced.

Why this exists
---------------
The run report's one rule is that the narrator arranges numbers and never computes them.
That rule is worth nothing if nobody checks it, and checking by eye fails exactly where it
matters — a plausible invented figure looks like a real one.

What counts as traceable
------------------------
A number in the report is accepted when, at the precision it was written to, it matches:

* a value anywhere in the facts JSON (0.726 <- 0.7259465947073727);
* that value as a percentage (18.4 <- 0.18433333);
* the length of any list in the facts (13 <- the 13 feature names);
* a difference between two AUC-like values, which the prompt explicitly permits as a
  restatement (0.04 <- 0.7697 - 0.7259);
* a small ordinal or year — rank numbers, "top 3", dates. Those are structure, not claims.

Anything else is reported. A false positive here is cheap (add the figure to the facts);
a false negative is a fabricated number shipped to a reader.

No third-party imports.
"""

import json
import re
import sys


def numbers_in(node, out):
    """Every numeric value anywhere in the facts, plus list lengths and the numbers
    embedded in the facts' own strings.

    That last one matters: a column is named `escalations_90d`, and a report glossing it
    as "escalations in the last 90 days" is restating the field name rather than inventing
    a window. The number is in the facts — it is just inside a key rather than beside one.
    """
    if isinstance(node, dict):
        for key, value in node.items():
            numbers_in(key, out)
            numbers_in(value, out)
    elif isinstance(node, list):
        out.add(float(len(node)))
        for value in node:
            numbers_in(value, out)
    elif isinstance(node, bool):
        pass
    elif isinstance(node, (int, float)):
        out.add(float(node))
    elif isinstance(node, str):
        for found in re.findall(r"\d+", node):
            out.add(float(found))


def allowed_values(facts):
    base = set()
    numbers_in(facts, base)

    allowed = set(base)
    for value in base:
        allowed.add(value * 100.0)      # a fraction restated as a percentage
        allowed.add(value * 1000.0)     # and as a per-mille, which the null rates use
    # Differences between values of similar magnitude — the prompt permits stating the
    # gap between the two AUCs, and that is the only subtraction it allows.
    small = [v for v in base if 0.0 <= v <= 1.0]
    for left in small:
        for right in small:
            if left > right:
                allowed.add(round(left - right, 6))
    return allowed


def precision_of(text):
    return len(text.split(".")[1]) if "." in text else 0


def traces(literal, allowed):
    text = literal.replace(",", "")
    try:
        value = float(text)
    except ValueError:
        return True
    # Structure rather than claims: ranks, small counts, years.
    if value == int(value) and (abs(value) <= 24 or 1900 <= value <= 2100):
        return True
    digits = precision_of(text)
    for candidate in allowed:
        if round(candidate, digits) == round(value, digits):
            return True
    return False


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    report = open(sys.argv[1]).read()
    facts = json.load(open(sys.argv[2]))
    allowed = allowed_values(facts)

    # Strip fenced code and inline code — those quote commands and JSON, not claims.
    body = re.sub(r"```.*?```", "", report, flags=re.S)
    body = re.sub(r"`[^`]*`", "", body)
    # And link targets, which carry issue numbers and dates.
    body = re.sub(r"\]\([^)]*\)", "]", body)

    untraced = []
    for match in re.finditer(r"(?<![\w.])(\d[\d,]*(?:\.\d+)?)(?![\w])", body):
        literal = match.group(1)
        if not traces(literal, allowed):
            line = body[: match.start()].count("\n") + 1
            context = body.split("\n")[line - 1].strip()[:90]
            untraced.append((literal, context))

    if untraced:
        print("UNTRACED NUMBERS — each of these must appear in the facts artifact:\n")
        for literal, context in untraced:
            print("  %-14s %s" % (literal, context))
        print("\n%d figure(s) could not be traced. Either they were computed while writing"
              "\n(which the report's one rule forbids) or the facts artifact is missing a"
              "\nfield it should carry." % len(untraced))
        return 1

    print("OK — every number in the report traces to the facts artifact.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
