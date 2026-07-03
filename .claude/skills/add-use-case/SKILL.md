---
name: add-use-case
description: Build a new catalog use case (UC-N) in the zetta engine — the ML/AI use-case creation recipe. Use when implementing a use case from the blueprint (churn's siblings - forecasting, lead scoring, triage, anomaly...), when asked to "add a use case", or when reviewing use-case work. Enforces the invariant shape (compose existing featurize→train→explain→rank primitives; new ML math lands in ml/ as a reusable op with its own oracle), the fixture-with-traps rule (every use case names its FedEx test), the canonical runner, and the four test kinds including an MCP journey.
---

# add-use-case — the UC-N engine recipe

Full recipe: **[docs/use-cases/creating-a-use-case.md](../../../docs/use-cases/creating-a-use-case.md)**.
The upstream flow (research prompt → blueprint UC-N artifact → issues →
worktree plan) is amaro-bridge's `add-use-case` skill + its runbook — run that
first; this skill is the zetta-side build. UC-1 churn is the worked example
for every step (`docs/use-cases/`).

## The two laws

1. **Compose, don't invent.** A use case is configuration of the shared
   engine (featurize spec → train → explain → rank/act). If UC-N needs math
   the engine lacks, that op lands in `ml/` as a reusable, oracle-proven
   slice FIRST — never inside the use case.
2. **Name the FedEx test.** Every use case must define its
   plausible-but-wrong domain failure (churn's: conflating FedEx
   subsidiaries) and encode it: the fixture carries the trap, a gate `Check`
   fails on it, and the E2E proves the trap blocks deploy.

## The build (each ≈ one PR)

1. Homes: `zetta::usecases::<name>` across include/src/tests/docs (+ README
   indexes, code-map).
2. Fixture: committed, small, schema-validated, no foreign runtime; carries
   the leakage trap (post-`as_of` events) + the FedEx-test trap.
3. Featurize spec (existing DDF composition; static mode for pre-engineered
   columns).
4. Gate check for the trap (the `Check` seam).
5. Step executor mapping connect→…→act to real work, each heavy step a
   recorded ComputeJob; reuse the near-data jobs so partitioned scale is free.
6. SolutionCatalog blueprint wired through THE canonical runner (#637).
7. Playbook policy (driver→action→owner; numbers frozen in code; constrained
   narrator + offline fallback).
8. Tests ×4: boundary units · gated+governed E2E (incl. the negative) ·
   testkit case + runbook · an MCP journey (docs/testing/mcp-journeys.md).
9. Docs page (domain framing, pipeline mapping, the trap, deferred pieces);
   blueprint/issues sync via zetta-sync.

**Done** = point at data → working, explained UC-N result, deployed + gated +
governed, distributed over partitions, drivable from GUI/console/MCP, trap
provably blocked, engine untouched.
