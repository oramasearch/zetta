# Personas

Four personas from the blueprint (PER-1..4); every surface decision should
name which of them it serves.

## PER-1 — the bottlenecked platform/AI lead (primary)

Owns the path from prototype to production; every team's data and AI requests
queue behind theirs, so they are the bottleneck however hard they work. Needs: **governed
self-service** their developers run without them (the golden path), speed
they can defend, and infrastructure that lives inside their walls. Buys the
substrate; judges us on the correctness gate and the audit trail. Surfaces:
deploy + governance panels, the control surface for their platform tooling.

## PER-2 — the data scientist / ML engineer

Modeling is the smallest part of shipping a model; the rest of their quarter
is plumbing. Needs: their artifacts (SQL, notebooks, workflows) ingested and
productionized without rewrites; explainability they can defend (drivers,
calibration, lineage). Surfaces: the graph workspace, the console, runs +
models panels, sim-Python.

## PER-3 — the waiting analyst / business team

"Just tell me what to focus on today" — grounded answers within governed
boundaries, not another dashboard to build. Needs: ranked, explained,
actionable outputs (the at-risk list with drivers and plays), one guided path
from question to answer. Surfaces: Home, search, the served endpoints their
existing tools call.

## PER-4 — the agent (first-class)

An LLM operating the cluster on a human's behalf. Wastes most of its cycles
on administration unless the surface is designed for it. Needs: everything in
[experience/ax.md](experience/ax.md) — full tool parity, progressive
disclosure (prompts + llms.txt), the spec index, traces threaded back,
errors it can act on.
