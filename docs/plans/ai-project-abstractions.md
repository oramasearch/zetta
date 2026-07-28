# Plan — the AI Project abstraction hierarchy, and Experiences

**Status:** design, not started. Captured at the end of a working session so it can be resumed cold.
**Goal it serves:** [#948](https://github.com/oramasearch/zetta/issues/948) — bring your data, ask in
plain language, get a model and a report you can interrogate.
**Next action:** finish the design interview (below), then open the design issue, then build.

## What this is

A system that builds AI systems needs abstractions that wrap the concepts, so generating one of
these systems is composition rather than bespoke engineering each time. The top-level container is
an **AI Project** (a.k.a. Data Project): it holds AI pipelines with trained models, data cleaning,
feature engineering, report generation, and lets a chat agent introspect and interact with it at
any project state.

## Source material

Two specs, authored outside this repo and **the source of truth for the hierarchy** until they land
here:

- `~/Downloads/system-design.md` (832 lines) — the reasoning. Evals vs benchmarks, where RLVR sits
  relative to both, how an eval becomes a verifiable reward, and the level-by-level derivation of
  the hierarchy from ContextSnapshot down.
- `~/Downloads/full-spec.md` (266 lines) — the settled spec. Overview, design principles, the full
  abstraction hierarchy, a component reference, the RLVR integration point, the generator
  configuration object, **Graphviz DOT diagrams**, and a list of resolved ambiguities.

**First task on resuming: copy both into this repo** (`docs/architecture/` or `docs/plans/`), since
a plan whose source lives in a Downloads folder is not reproducible.

## The hierarchy (from full-spec.md)

```
ContextSnapshot
 └─ ProjectAdvisor → ProjectBrief
      └─ AIProject
           ├─ ProjectPolicy
           ├─ ProjectKnowledgeBase (backed by LineageGraph)
           ├─ ProjectImprovementFramework
           │    ├─ CrossWorkspacePatterns
           │    ├─ PortfolioRecommendations
           │    └─ AdvisorFeedbackLoop → (updates ContextSnapshot / ProjectBrief)
           └─ ModelWorkspace[]
                ├─ ExperimentTracker
                ├─ ModelRegistry
                ├─ TrainingFramework (current + history)
                │    ├─ ModelDesignFramework
                │    │    ├─ ArchitectureSpec · LearningObjective · TrainingRegimen
                │    │    ├─ OptimizationConfig
                │    │    └─ RewardPolicyDesign (refs VerifierRegistry, when RL-based)
                │    ├─ AgentHarnessDesignFramework (optional)
                │    │    ├─ ToolRegistry · OrchestrationPolicy · MemoryDesign
                │    │    ├─ GuardrailPolicy · InteractionContract
                │    ├─ DatasetFramework
                │    │    ├─ SourcingPlan · PreparationPipeline · SplitPolicy · DatasetCard
                │    ├─ EvaluationFramework
                │    │    ├─ BenchmarkSet · EvalSuite (→ TestCases) · VerifierRegistry
                │    └─ Run[] → ModelArtifact + RunReport + EnvironmentSnapshot
                ├─ DeploymentFramework (RolloutPolicy · ServingConfig)
                └─ MonitoringFramework (DriftDetector · …)
```

**Naming has wiggle room.** These names need not match what the engine already calls things. Where
an existing type covers a concept, prefer the existing name and note the mapping; where the spec
draws a distinction the engine lacks, take the spec's name. What must not happen is two names for
one concept with no recorded mapping.

Known overlaps to reconcile on resume:

| Spec | Likely existing | Note |
|---|---|---|
| `AIProject` | `Project` (`project:` CRDT key) | almost certainly the same thing |
| `Run` → `RunReport` | `Run` (`run:` key) | RunReport may be the missing "report" artifact of #948 step 4 |
| `DatasetFramework` | `connect::` + `system.datasets` | sourcing/preparation partly exists |
| `EvaluationFramework` / `VerifierRegistry` | `AdmissionCase` grader + `checks::Check` | the gate is a verifier registry in all but name |
| `InteractionContract` | `agent::StudioAction` (#918) | the shipped contract |
| `ModelRegistry` | `ml::Model` (`model:` key) + `AgentVersion` | |
| `AgentHarnessDesignFramework` | `agent::Sandbox` + `AgentRecord.tools/goals` | |
| `RewardPolicyDesign` | `agent::Goal` + `goal_reward_*` (#941) | goals already ARE the reward policy |

## Experiences — the piece that is NOT yet designed

**The ask, verbatim:** *"we also want to create **Experiences** which are like Roblox experiences —
for business users to interact with the AI project (e.g. reports, data apps, etc.)"*

The analogy is load-bearing and worth taking seriously: a Roblox experience is a **published,
self-contained, interactive thing** that someone *enters* — not a document they read. It is built
on a platform, it has an author and an audience, it can be shared, discovered, and run without the
audience knowing anything about how it was made. Applied here: a business user should be able to
enter something built on top of an AI Project — a report, a data app — and use it without touching
pipelines, models, or datasets.

**This is under-specified and the design interview was cut short by context.** Open questions to
resolve BEFORE writing the issue — the interview should ask these one at a time, each with a
recommendation:

1. **Is an Experience a first-class entity or a view?** Recommendation: first-class, replicated
   (`experience:` key), because it has an author, an audience, permissions, and a lifecycle
   independent of any one run.
2. **What is the relationship to an AI Project?** One project → many experiences? Can an experience
   span projects? Recommendation: many-per-project, one project each, until a case for spanning
   appears.
3. **What kinds are there at v1?** Report · data app · dashboard · chat surface? Recommendation:
   two — **Report** (generated, refreshed by runs) and **Data App** (interactive, user input →
   inference) — because those are the two the ask names.
4. **Who builds one, and how?** Authored by the agent from plain language? Assembled by a technical
   user? Templated? Recommendation: agent-generated from a request, then editable — matching how
   pipelines already work.
5. **What does a business user actually DO in one?** Read numbers · filter · score a new row ·
   ask a question in chat · trigger an action? This is the one most worth grilling.
6. **How does it stay current?** Bound to a Run (frozen), or to a Project (live, refreshes when a
   run completes)?
7. **Permissions.** Who can enter? Does entering an Experience grant read on the underlying dataset
   — and if not, how does a report show numbers derived from data the viewer cannot query?
8. **Publishing / discovery.** Is there a catalogue? Shareable link? The Roblox analogy implies
   discovery matters.
9. **Does an Experience embed the chat agent**, scoped to that project — which would make it the
   surface where #948 step 5 (inspect the data, run inference, introspect results) actually lands?

## Deliverables

- [ ] Copy both specs into the repo; they are currently only in `~/Downloads`.
- [ ] Finish the Experiences interview (questions above, one at a time, each with a recommendation).
- [ ] **Graphviz diagrams, generated** — `full-spec.md` already carries DOT source. Render to SVG
      and commit alongside, following the `zetta-docs` precedent (pre-rendered Graphviz SVGs,
      generated not hand-edited).
- [ ] Open the design issue supporting [#948](https://github.com/oramasearch/zetta/issues/948),
      carrying: the hierarchy, the naming-reconciliation table above, Experiences, and the diagrams.
- [ ] Reconcile spec names against existing types and record the mapping in the issue.

## Why this matters to #948

[#948](https://github.com/oramasearch/zetta/issues/948) says what the *user experience* must be:
bring data, ask in plain language, get a model and a report, then interrogate it in conversation.
This plan is the *structure* that makes that composable rather than bespoke — and **Experiences are
plausibly the missing surface for #948 step 5**, where a business user interrogates what the
pipeline produced without touching the pipeline.

Related: [#948](https://github.com/oramasearch/zetta/issues/948) ·
[#947](https://github.com/oramasearch/zetta/issues/947) ·
[#941](https://github.com/oramasearch/zetta/issues/941) ·
[#854](https://github.com/oramasearch/zetta/issues/854)
