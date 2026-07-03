# Creating a use case — the engine recipe

How a new catalog use case (UC-N) is built in THIS repo. The upstream flow
(deep-research prompt → blueprint artifact → issues → worktree plan) lives in
amaro-bridge's `add-use-case` skill + the
[adding-a-use-case runbook](https://github.com/oramasearch/amaro-bridge/blob/main/docs/runbooks/adding-a-use-case.md);
this page is the zetta-side complement: what actually gets written, where, and
what "done" means. UC-1 churn is the worked example throughout — a new use
case is deliberately a **re-instantiation of its shape**, not a new
architecture.

## The invariant shape

A use case is a **generic AI-project type** (ADR-25/26): the same template any
team runs on its own data. Its engine form is a composition of existing
primitives — **featurize (point-in-time) → train → explain → rank/act** — with
use-case-specific *configuration*, not use-case-specific *math*:

- **New ML math goes in `ml/` as a reusable op** (the way GBDT/TreeSHAP/
  calibration are shared), never inside a use case. If UC-N needs an op the
  engine lacks (e.g. a forecaster), that is its own `ml/` slice with its own
  oracle — the use case then composes it.
- The use case owns: its dataset shape + fixture, its featurize spec, its
  step-executor mapping, its gate checks, its playbook policy, its docs/tests.

## The checklist (each item ≈ one PR)

1. **Namespace + homes**: `zetta::usecases::<name>` — `include/zetta/usecases/
   <name>/`, `src/core/usecases/<name>/`, `tests/usecases/<name>/`,
   `docs/use-cases/<name>.md` (+ index line in the README).
2. **Synthetic fixture** (the UC-1 pattern, `churn-dataset.md`): committed,
   small, schema-validated at the boundary, **no foreign runtime**, and
   carrying the use case's *traps* — the deliberate leakage case (events after
   `as_of`) and the correctness-defining case (churn's FedEx entity-resolution
   analog: the domain mistake that must fail the gate). Every use case must
   name its "FedEx test" — what does *wrong but plausible* look like here?
3. **Featurize spec**: express the feature table as the existing DDF
   composition (as-of windows, group-bys, joins; leakage-free by
   construction). Static/engineered-columns mode if the data arrives
   pre-featurized.
4. **Gate checks**: the generic checks (schema/type, leakage) come free;
   add the use-case eval as a `Check` (the seam) that fails the gate on the
   trap from step 2.
5. **Step executor**: map `connect→clean→model→index→serve→act` to real work
   (the `ChurnStepExecutor` pattern — each heavy step a recorded
   `ComputeJob`); reuse the near-data jobs so it scales over partitions for
   free.
6. **Catalog blueprint**: a `SolutionCatalog` entry (schema-validated) wired
   to the real engine through THE canonical runner (#637) — the same graph
   deploys and runs, one-button, from the Deploy panel, the console, and MCP.
7. **Act layer**: the playbook policy (driver → action → owner + save-rate
   prior), numbers computed + frozen in code, narrator constrained (never
   invents numbers), offline fallback.
8. **Tests, all four kinds**: unit (spec/fixture boundaries) · the E2E
   (fixture → ranked, explained, gated, governed result — the
   `test_uc1_end_to_end` pattern, including the negative: the trap blocks the
   gate) · a testkit case + runbook (GUI-runnable) · an **MCP journey**
   ([../testing/mcp-journeys.md](../testing/mcp-journeys.md)) so an agent can
   drive UC-N end to end.
9. **Docs**: the use-case page states the *domain* framing (who runs this,
   what decision it feeds), the pipeline mapping, the trap, and the
   deliberately-deferred pieces. Blueprint + issues stay in sync via
   zetta-sync.

## Done means

Point at data → a working, explained UC-N result — deployed, gated, governed —
running distributed over partitions, drivable from the GUI, the console, and
MCP, with the trap provably blocked. The engine carried forward untouched; the
catalog grew by one.
