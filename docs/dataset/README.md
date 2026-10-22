# dataset — native training-dataset generation

`zetta::dataset` synthesizes **admission-gated training corpora inside Zetta** (#873/#878/#879),
grounded in the solutions-discovery plan — the native, in-engine replacement for the offline
Python generators (`scripts/gen_*_corpus.py`, ADR-17), and what a per-customer fork (#853) uses
to grow its own corpus from its own data.

## The seam

- **`DatasetGenerator`** (`include/zetta/dataset/dataset_generator.hpp`) — one implementation per
  dataset kind. `generate(spec)` returns a `GeneratedDataset` (gated `AdmissionCase`s + the plan
  subjects covered). Every case admits: reference passes, negative controls fail — *discrimination
  is the product* (blender-rl).
- **`DatasetGeneratorFactory`** (`dataset_generator_factory.hpp`) — registers every generator;
  callers create one by kind. The `generate_dataset` / `list_dataset_generators` capabilities and
  the GUI + web "Data generators" view all drive the factory, so a new generator (one line in
  `builtin()`) shows up in API + MCP + web + GUI at once.

## Generators

- **`BaseAgentDatasetGenerator`** (`base_agent_generator.cpp`) — the base agent's intent→pipeline
  corpus in the `{message, tool_calls, execution_graph}` action shape. Coverage is **data**:
  `plan_solution_templates()` (`plan_templates.cpp`) is a table of plan subjects → the facts their
  correct pipeline must satisfy (mined from the plan's per-method graphs + per-solution gate
  checks); `build_solution_graph` (agent domain) turns each into a real gradeable pipeline. Covers
  the 24 use cases, 21 methods, 12 solutions, DSC, autonomy, and abstain.
- **`AutoSuggestDatasetGenerator`** (`auto_suggest_generator.cpp`) — the Auto-Suggest model's
  corpus (#554): a data state → suggest the present use cases / withhold the absent (distractors),
  gated on the same discipline.

## The held-out canary

`DatasetGenSpec::held_out_fraction` reserves a fraction of **whole subjects** — not
resampled variants of a subject the model already saw — so the canary tests
generalization rather than recall. `held_out_subject_count` picks the **trailing**
subjects (deterministic, no RNG, per the reproducibility rule), a non-zero fraction
always reserves at least one subject, and it never reserves them all.

**The convention:** held-out cases are the **last `held_out` entries of `cases`**, and
their subjects are the last entries of `subjects`. Train on `cases[0 .. size()-held_out)`;
evaluate on the rest. Every generator must honor this — a corpus that returns
`held_out == 0` for a non-zero fraction is silently training without a memorization check.

## Work ceilings

A generator runs on the node's owner thread and holds the whole corpus in memory while it is
gated and serialized, so a spec is bounded before anything is expanded (#1398). The content of
a case is engine-authored; the only caller-controlled multiplier is
`DatasetGenSpec::variants_per_subject`.

- **`kMaxVariantsPerSubject`** (16): the most copies of one subject a spec may ask for. Below 1
  means 1. `generate_dataset`'s schema carries it as `maximum`, so the capability answers
  `invalid_input` at `/variants_per_subject` before the handler runs.
- **`kMaxExpandedCases`** (2048): subjects × variants, checked by division so no count can
  overflow it. It keeps the bound as the plan catalogue grows.
- **`bounded_variants(spec, subject_count, &reason)`** is the one check; every synthesizing
  generator calls it before building a variant. A refused spec comes back with
  `GeneratedDataset::error` set and no cases, and the capability and the GUI report that reason
  instead of gating an empty corpus.

The interaction families add a fixed set of cases the spec cannot multiply, so a corpus is at
most its one-variant size plus `kMaxExpandedCases`.

## Related

- The action + solution-graph bridge it builds on: `zetta::agent::action` /
  `zetta::agent::solution_graph` (agent domain). The gate: `admit_corpus` (`agent/admission_gate.hpp`).
- Cards: [`../agent/cards/`](../agent/cards/README.md). Coverage:
  [`../agent/training-coverage-checklist.md`](../agent/training-coverage-checklist.md).
- Issues: #873 (epic) · #878 (base-agent coverage) · #879 (Auto-Suggest) · #866 (synthesis) ·
  #853 (per-customer fork). Capabilities: `docs/api/`.
