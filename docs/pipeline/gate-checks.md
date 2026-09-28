# Graph correctness checks

The concrete correctness checks that plug into the
[correctness gate](gate.md). They implement
the gate's `Check` interface (`include/zetta/pipeline/graph_checks.hpp`,
`src/core/pipeline/graph_checks.cpp`) and turn a **mistyped or leaky** execution graph into
a clear-reason gate **failure** that blocks deploy (RULE-1), while letting a clean
graph through.

The reusable annotation and methodology-specific checks are:

| Check | Name | What it catches |
| ----- | ---- | --------------- |
| `SchemaTypeCheck`  | `schema_type`  | a step that requires a field no upstream source provides, or provides under a different type |
| `DataLeakageCheck` | `data_leakage` | an ML step whose feature uses data dated **after** its as-of cutoff (point-in-time violation), or that **is / derives from the label** (target leakage) |
| `SnapshotLineageCheck` | `snapshot_lineage` | a feature-bearing model whose lineage ends in a live table or future snapshot, or whose label window violates its configured blackout gap |
| `TemporalValidationCheck` | `temporal_validation` | a time-ordered model without time-aware validation |
| `EntityResolutionGraphCheck` | `entity_resolution_profile` | an M-2 model that omits blocking keys, valid match/review thresholds, or verified-identifier conflict rejection |
| `SegmentationCheck` | `segmentation` | entity/outcome features, non-global preprocessing, an incorrect K criterion or minimum share, malformed parameters and disconnected assignment/profile outputs |
| `ForecastGraphCheck` | `forecast_correctness` | an M-4 graph with invalid temporal backtests, future features, calibration, or no seasonal-naive baseline |
| `SurvivalCensoringCheck` | `survival_censoring` | a survival model that does not declare censoring, stop-after-event, duration/event columns, and point-in-time features |
| `CausalUpliftCheck` | `causal_uplift` | an M-6 step missing treatment/outcome roles, weakening overlap bounds, or using an outcome-like or post-treatment feature |
| `AnomalyGraphCheck` | `anomaly_correctness` | an M-7 graph with invalid budget, label use, robust scaling, or causal windows |
| `NextBestActionGraphCheck` | `next_best_action_correctness` | an M-8 graph that drops chronological holdout, guesses logging propensities, treats predictive propensity as causal lift, or weakens eligibility/contact-cap constraints |
| `RankCorrectnessCheck` | `rank_correctness` | a rank model without a group, a whole-group split, or a ranking-time feature contract |
| `LanguageGroundingCheck` | `language_grounding` | an M-10 graph that does not require citations on every answer sentence or does not refuse empty retrieval |
| `LanguageTrainingAccessCheck` | `language_training_access` | an M-10 graph whose source and model do not require runtime caller-read authorization |
| `DocumentReviewCheck` | `document_review` | document extraction without confidence, source evidence, a valid review threshold, and a downstream review queue |
| `GraphEdgeTimeCheck` | `graph_edge_time` | an M-12 graph step without a finite prediction cutoff and matching enabled edge-time filter |
| `VisionSourceSplitCheck` | `vision_source_split` | a vision model without a named source-disjoint holdout |
| `SpeechConsentCheck` | `speech_consent` | a speech model that does not require an explicit boolean consent field |
| `SpeechPiiRedactionCheck` | `speech_pii_redaction` | a speech model that could store unfiltered transcript text |
| `SpeechLocalAudioCheck` | `speech_local_audio` | a speech model that permits audio to leave its owner node |
| `BanditPolicyCheck` | `bandit_policy` | an M-15 graph without a positive exploration floor, with a cap below that floor, or with declared arms whose floors/caps cannot sum to one |
| `OptimizationContractCheck` | `optimization_contract` | an M-16 graph that allows point forecasts, skips independent constraint verification, or omits solver status and MIP gap reporting |
| `S1*Check` family | `s1_point_in_time`, `s1_label_window`, `s1_temporal_validation`, `s1_group_split`, `s1_censoring`, `s1_calibration`, `s1_entity_resolution`, `s1_leakage_review`, `s1_level_composition` | an S-1 graph with future/live features, overlapping windows or groups, random validation, incomplete/censored labels, uncalibrated sampling, duplicate entities, leaky features, suspicious AUC, or a missing level stage |
| `S2*Check` family | `s2_point_in_time_conversion`, `s2_post_qualification_leakage`, `s2_capacity_routing` | an S-2 graph with live/future CRM features, incomplete or overlapping conversion labels, post-qualification predictors, or routing without current owner, territory, and capacity evidence |
| `S3*Check` family | `s3_short_tenure_censoring`, `s3_value_horizon`, `s3_entity_accounting` | an S-3 graph that treats immature value as complete, omits or mismatches its prediction horizon, or can count one customer more than once |
| `S4*Check` family | `s4_regulatory_contact`, `s4_vulnerable_customer`, `s4_promise_to_pay_point_in_time` | an S-4 graph missing regulatory contact limits, vulnerable-customer exclusion, or point-in-time promise labels |
| `S12*Check` family | `s12_no_automated_denial`, `s12_fairness_review`, `s12_document_confidence`, `s12_level_composition` | an S-12 graph that permits an automated denial, omits owned fairness review, routes low-confidence document fields without review, or does not match its registered level composition |

| `UncertaintyValidationCheck` | `uncertainty_validation` | an M-17 model that assumes coverage instead of measuring it, omits its exchangeability assumption, uses an invalid nominal coverage, or allows conformal calibration rows to overlap fitting rows |

| `EnsembleOutOfFoldCheck` | `ensemble_out_of_fold` | an M-18 step that asks the meta-model to fit in-sample scores, uses fewer than two folds, skips the full-training base refit, or uses non-temporal folds with a time column |
| `LabelEfficiencyGoldIsolationCheck` | `label_efficiency_gold_isolation` | an M-19 graph that treats weak labels as gold or does not reserve a disjoint gold split |

| `PrivacyBudgetCheck` | `privacy_budget` | an M-20 model with a missing or invalid epsilon/delta maximum, unknown noise mechanism, invalid public clipping bounds, or disabled privacy evaluation |

| `StreamingCorrectnessCheck` | `streaming_correctness` | an M-21 model without event-time ordering, allowed lateness, future-event buffering, delayed-label protection, or prequential evaluation |

All are registered in [`default_gate()`](gate.md) (after `NonEmptyGraphCheck`),
so every gate run applies them. They are **opt-in per step**: a step that carries
none of the relevant annotations is skipped and passes, so adding them never
penalises a graph that doesn't use them.

### S-1 composed solution checks

The S-1 checks evaluate the full composed graph once, before the ordinary gate
checks evaluate each methodology stage. They engage only when the solution
manifest names `S-1` and carries its bound foundation. This preserves each
methodology's native parser while allowing cross-stage assertions.

`s1_point_in_time` uses the snapshot time parser for every methodology Connect
source and refuses a live source, a missing cutoff, a different time
representation, or a cutoff after T. `s1_label_window` enforces the blackout
gap. `s1_temporal_validation` requires an out-of-time split, at least two rolling
windows, and an explicit stability result. `s1_group_split` requires a named,
disjoint customer group.

`s1_censoring` requires fully observed propensity labels and, from level 2, the
censoring-aware M-5 stage. `s1_calibration` requires a disjoint Platt or isotonic
calibration set whenever sampling is used. `s1_entity_resolution` requires M-2,
one actionable unit, and duplicate refusal. `s1_leakage_review` applies the
configured blocklist and refuses reported AUC above its default 0.95 threshold.
`s1_level_composition` requires M-5 at level 2, M-6 with at least one randomized
cycle and positive-only/no-contact targeting at level 3, and M-8 at level 4.

The profile is created from the snapshot, dataset roles, and optional
`sources.correctness` settings during `create_solution`. The same bound graph is
checked again by `run_solution`. See [S-1](../solutions/S-1.md) for the operator
contract and refusal list.

### S-2 composed solution checks

The S-2 checks engage only for a bound S-2 manifest and evaluate the composed
graph once. `s2_point_in_time_conversion` requires every methodology source to
end in a snapshot at or before scoring time, a named fully observed conversion
label, and a label window after the feature cutoff plus blackout gap.
`s2_post_qualification_leakage` requires a nonempty CRM blocklist and refuses
any selected feature on it. `s2_capacity_routing` requires named owner,
territory, and capacity columns, capacity-aware routing, and no-contact behavior
when capacity is unavailable. It also requires M-2 and M-3 at every level and
M-6 at levels 3 and 4.

`create_solution` builds the profile from the snapshot, dataset roles, and
optional `sources.correctness` overrides. `run_solution` checks the stored
profile again. See [S-2](../solutions/S-2.md) for its data, policy, action, and
measurement contract.

### S-3 composed solution checks

The S-3 checks engage only when the solution manifest names `S-3`. They read a
bound profile produced from the caller's dataset roles, M-1 horizon, and
optional `sources.correctness` overrides. The profile is stored with the graph,
and the same checks run during creation and execution.

`s3_short_tenure_censoring` requires a named tenure column, a positive minimum
observed tenure, `right_censor` handling, and explicit application to training
labels. Level 2 and above must also contain the M-5 survival stage.
`s3_value_horizon` requires a positive value horizon and nonempty net-value
label, then verifies that M-1 is a squared-error regression over that exact
target and horizon. `s3_entity_accounting` requires the M-2 stage, a stable
entity key, one-value-per-entity aggregation, and duplicate refusal. See
[S-3](../solutions/S-3.md) for its data and operating contract.

### S-4 composed solution checks

S-4's three checks precede S-12 in registration order. They require explicit
regulatory eligibility and contact limits, exclusion of vulnerable customers,
and frozen promise-to-pay labels that exclude future updates. See
[S-4](../solutions/S-4.md) for the policy and data contract.

### S-12 composed solution checks

The four S-12 checks engage only for a solution manifest bound to `S-12`.
They require every denial outcome to remain A0, an owned fairness review with
protected fields excluded from routing features, M-11 confidence evidence with
low-confidence review routing, and exactly the methodology stages registered
for the selected level. The policy constraints are checked again immediately
before `run_solution` acts. See [S-12](../solutions/S-12.md) for the complete
operator contract and refusal list.

`SegmentationCheck` is mandatory for M-3 model steps, even with no other annotations.
It validates the nested `segmentation` config against `segmentation_config_schema()`
and requires matching preparation/model contracts. Its implementation and refusal
tests live in `src/core/ml/segmentation_graph.cpp` and
`tests/ml/test_segmentation_methodology.cpp`. See the full
[segmentation correctness rules](../ml/segmentation.md#running-it).

`ForecastGraphCheck` (`forecast_correctness`, #1043) is also registered in the
default gate. It applies only to forecast model kinds, validates the complete
M-4 graph and model-attribute schema, and refuses random validation, future
features, probability calibration, invalid horizon limits, or omission of the
seasonal-naive baseline. Runtime checks additionally validate each series' real
history length and ownership. The full contract and refusal tests are documented
in [forecast.md](../ml/forecast.md).
### `next_best_action_correctness` — recommendation policy safety

This check engages only when a model step names an M-8 model kind. It parses the
complete five-stage pipeline through the same contract used by the initiator and
coordinated peers. It requires the time-based interaction holdout, the explicit
logging-propensity rule, the predictive-not-causal warning, and an Act step that
applies eligibility and contact caps as hard constraints before budgeted
best-action selection. Missing stages, extra transforms, altered outputs, unknown
attributes, overlapping column roles, and invalid training parameters fail with a
specific reason.

### M-10 grounding and training access

The language checks engage only when a model step names `model_kind: language`
or another M-10 alias. `language_grounding` requires boolean
`citations_required` and `refuse_without_retrieval` attributes on the model.
`language_training_access` requires `governance_check` on that model and
`caller_read_required` on an upstream connect step. The runtime then authorizes
every effective entity, label, feature, text, query, and relevant-id column
before it reads rows. These graph flags do not replace runtime authorization;
they make an edited plan that omits it fail verification.
### `ensemble_out_of_fold` — honest stacking

This check engages only for `ensemble`, `ensembling`, and `stacking` model
kinds. It requires `stacking_predictions: "out_of_fold"`, `folds >= 2`, and
`base_refit: "all_training"`. When `time_column` is non-empty it also requires
`fold_strategy: "time_ordered"`. Each refusal names the required attribute, so
an edited M-18 graph cannot silently turn its meta-model training into an
in-sample fit or validate earlier observations from later data.
### `label_efficiency_gold_isolation` — weak labels are not gold

This check engages on `label_efficiency`, `active_learning`, and
`weak_supervision` model steps. It requires `weak_labels_are_gold: false` and
`gold_split: "disjoint"`. Missing, malformed, or contradictory values fail the
gate before a run starts. The runtime uses the same rule when materializing the
label queue and weak-label table; both exclude every reserved gold row.
### `streaming_correctness` — M-21 event and label order

This check engages only on model steps whose `model_kind` is `streaming` or
`online_learning`. It requires `event_time_ordered: true`, a non-negative
`allowed_lateness`, `future_event_policy: "buffer_until_watermark"`,
`label_time_policy: "not_before_event_time"`, and `prequential: true`. Missing
or contradictory policy blocks the graph before run. Runtime processing applies
the same rules to each event; see [online and streaming learning](../ml/streaming.md).

## How a check reads a step

The default gate also registers `AnomalyGraphCheck` (`anomaly_correctness`,
`src/core/ml/anomaly_plan.cpp`, #1046). It engages on M-7 model kinds and validates
the complete executable shape. Missing or ambiguous alert policies, training on
evaluation labels, role/feature overlap, non-robust scaling, future-fitting KPI
settings, missing time roles, invalid bounds, grouping, and unsupported operations
or data flow are hard refusals. Each refusal has a regression in
`tests/ml/test_anomaly_methodology.cpp`; the same validator checks run overrides
before a Run is created. See [M-7 correctness rules](../ml/anomaly.md#correctness-rules-enforced-in-code).

A graph node's `attributes` bag (`zetta::ir::Node::attributes`,
[execution-graph.md](execution-graph.md)) is free-form JSON. These checks read a
small, **documented set of annotation fields** out of it. Everything else in the
bag (`source`, `table`, …) is ignored. The annotations are validated against a
JSON Schema **at the boundary** — see [below](#json-schema-at-the-boundary) — so a
malformed annotation fails fast with a precise reason instead of being silently
misread.

### `schema_type` — fields a step needs vs. the source profile

Two annotation fields drive it:

- **`schema`** — the fields a step **outputs** and their types: a *source profile*
  for a `Connect` step, the transformed shape for a `Clean` step. A map of field
  name → type token.
- **`requires`** — the fields a step **expects from its data-flow inputs**, and
  the type each must have. A map of field name → type token.

For every step that declares `requires`, the check gathers the union of the
`schema` maps of **all the step's transitive data-flow ancestors** (following
`DataFlow` edges back through the graph — intermediate steps that don't re-declare
a schema are transparent). Each required field must appear there with a matching
type:

- field absent from every ancestor profile → **fail** (`requires field "revenue"
  (float) but no upstream data source provides it`).
- field present but no provided type matches → **fail** (`requires field "revenue"
  as float but the upstream source provides it as string (type mismatch)`).

Type tokens are **opaque strings compared case-insensitively** (`Int` matches
`int `); the check does not interpret a type system, only equality of the names
the author writes.

### `data_leakage` — point-in-time and target leakage for ML steps

Engages on any step that declares a non-empty **`features`** array. Three fields:

- **`as_of`** — the point-in-time cutoff: every feature must be computed only from
  data available at or before this instant. ISO-8601 UTC date or date-time
  (`"2024-01-01"` or `"2024-01-01T00:00:00Z"`). **Required** when `features` is
  present — without a cutoff there is nothing to validate against, so a step that
  declares features but no `as_of` **fails**.
- **`label`** — the target column the model predicts (optional).
- **`features`** — each entry has a `name`, and optionally `data_as_of` (the
  latest instant of data the feature draws on), `derived_from`, and
  `leaks_target`.

A feature **fails** the check when:

- **point-in-time:** `data_as_of` is **after** `as_of` — the feature uses future
  data the model would not have at prediction time (`feature "spend_after" uses
  data as of 2024-02-01, after the as_of cutoff 2024-01-01 — point-in-time
  violation (data leakage)`). At the cutoff is allowed.
- **target leakage:** the feature **is** the label (`name` equals `label`), is
  **derived from** it (`derived_from` is the literal `"label"` or the `label`
  column name), or is explicitly flagged (`leaks_target: true`) — `feature
  "churn_proxy" is derived from the label ("churned") — target leakage`.

Date-only and date-time instants compare correctly (a date is treated as
midnight UTC), so a same-day afternoon feature leaks against a date-only cutoff.

### `snapshot_lineage` — immutable feature sources and window separation

Engages on model steps with a non-empty `features` array. The model must declare
`as_of`, `feature_window_end`, `label_window_start`, and a non-negative
`blackout_gap_seconds`. Every terminal data-flow ancestor must be a Connect step
with `source_kind: "snapshot"` and `snapshot_as_of <= as_of`; a live table is a
hard refusal. The feature window may not extend after prediction time, and the
label window must satisfy
`label_window_start >= feature_window_end + blackout_gap_seconds`.

The snapshot materializer and row-level join that produce these sources are
documented in [point-in-time snapshots](../solutions/point-in-time-snapshots.md).

### `entity_resolution_profile` — mandatory M-2 safety declarations

Every `entity_resolution` model step must declare a non-empty `blocking_keys`
array, numeric `review_threshold` and `match_threshold` values satisfying
`0 <= review < match <= 1`, and
`conflict_policy: reject_verified_identifier_conflicts`. A missing or weaker
declaration fails before execution. The runtime enforces the same thresholds
and checks whole clusters before each union, preventing a fuzzy bridge from
merging records across incompatible verified identifiers.

### `causal_uplift` — treatment design and pre-treatment features

Engages on model steps whose `model_kind` is `uplift` or `causal`. The step must
name distinct `treatment` and `outcome` columns. Optional `overlap_lower` and
`overlap_upper` annotations must remain within `[0.05,0.95]` and be ordered.
Features named like the outcome, equal to the treatment column, or carrying a
`data_as_of` later than `treatment_time` fail before execution.
An explicit `post_treatment: true` feature annotation also fails. Data-first run
planning reuses this check and `DataLeakageCheck` for the analyzed feature sources;
an accepted graph is not permission to train on newly discovered leakage (#1062).

### `graph_edge_time` — graph rows available at prediction time

This check engages only for `graph` and `graph_ml` model steps. The model must
declare `time_column` and a finite `prediction_time`. An enabled clean step must
feed the graph model through a data-flow edge and declare the same
`filter_time_column` and cutoff plus
`exclude_after_prediction_time: true`. Missing, disabled, or mismatched filters
fail the gate before the graph can run.

### `vision_source_split` — refuse frame-level evaluation

Engages on a Model step whose `model_kind` is `vision` or `computer_vision`.
It requires `source_disjoint: true` and a non-empty `source_column`. A missing or
false flag fails because random frame splitting can put nearly identical frames
from one camera, site, or document in both train and test. A missing source
column fails because the runner cannot know which rows must stay together.

The M-13 shape emits both annotations. The runtime also performs the split from
the named column, so the graph promise and the executed behavior agree.

### Speech correctness checks

Model steps with `model_kind: speech` or `audio` must set
`consent_required: true`, name a non-empty `consent_column`, set
`redact_pii: true`, and set `audio_local_only: true`. These declarations make a
compiled graph that weakens any M-14 privacy rule fail before execution. The
speech runner independently validates row consent before reading a blob and
filters text before table insertion; graph annotations are not treated as proof
that the underlying data complied.
### `uncertainty_validation` — honest uncertainty for M-17

Engages on a model step whose `model_kind` is `bayesian`, `probabilistic`, or
`uncertainty`. It requires:

- `coverage_checked: true`, because nominal coverage is a request rather than
  evidence;
- numeric `nominal_coverage` strictly between zero and one;
- a non-empty `exchangeability_assumption`; and
- an `uncertainty_kind` of `hierarchical`, `split_conformal_regression`, or
  `split_conformal_classification`.

A `split_conformal*` shape must additionally set
`calibration_disjoint: true`. This graph check is backed by the runtime split
validator, which rejects overlapping row indexes even if a hand-authored graph
claims they are disjoint.
### `privacy_budget` — an enforceable M-20 release

This check engages only on a model step with
`model_kind: "synthetic_private"`. It requires numeric `epsilon`, `delta`,
`maximum_epsilon`, and `maximum_delta`; requested values must be within the
stated maxima. Delta values must lie below 1, epsilon values must be positive,
and the Gaussian mechanism requires a positive delta.

The step must also name `noise` as `gaussian` or `laplace`, state numeric public
`clip_min` and `clip_max` bounds with `clip_min < clip_max`, and set
`privacy_evaluation: true`. This graph check makes the declared plan safe to
submit. The M-20 runner repeats budget and bound validation against the actual
run parameters and refuses publication if DCR or membership evaluation fails.
The full mechanism and report are in
[synthetic_private.md](../ml/synthetic_private.md).

## Example

A clean churn graph — the source declares its profile, the model declares the
typed fields it needs and features all dated before the cutoff:

```json
{
  "nodes": [
    { "id": "n-load", "kind": "connect", "name": "load_orders",
      "attributes": { "source": "postgres",
                      "schema": { "customer_id": "int", "revenue": "float",
                                  "signup_date": "timestamp" } } },
    { "id": "n-clean", "kind": "clean", "name": "dedupe" },
    { "id": "n-model", "kind": "model", "name": "churn_model",
      "attributes": {
        "as_of": "2024-01-01",
        "label": "churned",
        "requires": { "customer_id": "int", "revenue": "float" },
        "features": [
          { "name": "spend_2023", "data_as_of": "2023-12-31" },
          { "name": "tenure_days" }
        ] } }
  ],
  "edges": [
    { "from": "n-load",  "to": "n-clean",  "kind": "data_flow" },
    { "from": "n-clean", "to": "n-model",  "kind": "data_flow" }
  ]
}
```

The applicable checks pass and the gate allows deploy. Change `revenue` to `"string"` in the
source profile and `schema_type` fails; add a feature with `"data_as_of":
"2024-06-01"` and `data_leakage` fails — either blocks deploy with a reason.

### `bandit_policy` — safe allocation constraints

The bandit check engages on model steps whose `model_kind` is `bandit`,
`bandits`, or `sequential_decisioning`. Such a step must declare numeric
`exploration_floor` and `default_arm_cap` values. Every arm-specific cap must
be between the floor and one. When `arms` are declared on the graph, their
floors must fit within total probability one and their effective caps must be
able to carry total probability one. The runtime repeats these checks after
run-time arms are resolved from parameters or the input table.

```cpp
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/serialization.hpp"
using namespace zetta;

ir::ExecutionGraph graph = *ir::from_json(document);   // the compiled graph
ValidationReport report = default_gate().run(graph);   // runs the default checks
if (report.block_deploy) {
    // RULE-1: refuse to deploy; report.checks carries the per-check reasons.
}
```

## JSON Schema at the boundary

The annotation fields have a formal contract,
[`schemas/graph_step_annotations.schema.json`](../../schemas/graph_step_annotations.schema.json)
(JSON Schema draft-07). Each step's `attributes` bag is validated against it
**before** a check reads it (via `pboettch/json-schema-validator`, the same path
the [Run record](runs.md) uses); the runtime copy of the schema lives inline in
`src/core/pipeline/graph_checks.cpp` — **keep the two in sync**. Only the named fields are
constrained, so any other attribute is allowed; but a present field that is the
wrong shape (`requires` as an array, a `features` entry without a `name`, an
`as_of` that isn't an ISO-8601 instant) fails the check with the validator's
reason (`step "model" (n-model): malformed correctness annotations — /requires:
…`). This is the issue's *"a mistyped graph fails with a clear reason"* applied to
the annotations themselves.

## `optimization_contract` — no unchecked or point-forecast plan

An M-16 model step (`model_kind: "optimization"` or
`"operations_research"`) must set all three contract flags to `true`:

- `scenario_input`: the objective or constraints come from scenario or
  quantile predictions, never one point prediction;
- `verify_constraints`: the runner independently checks every bound and
  constraint after solving and suppresses any violating plan; and
- `report_solver_status`: every run reports termination status and MIP gap.

The methodology's own compiler emits these flags. An edited or imported graph
that removes any flag is refused before run. The runner enforces the same rules
again at the table and solution boundaries; the graph check makes the unsafe
shape impossible to deploy in the first place (#1055).

## Where it fits

These checks implement the `Check` interface the [gate](gate.md) defines, alongside the
trivial `NonEmptyGraphCheck`. The gate aggregates their `CheckResult`s into a
[`ValidationReport`](validation.md) and sets `block_deploy` — there is **no new
result format** here. The earlier UC-1 resolution-output check (`#312`) remains
alongside the M-2 profile check.

See also [gate.md](gate.md) (the runner), [execution-graph.md](execution-graph.md)
(the graph + the `attributes` bag), and [validation.md](validation.md) (the report
type), and the related artifacts: ENT-5, FEAT-3 (`#134` / `#301`), RULE-1.

## Learning-to-rank correctness

The `rank_correctness` check applies to model kinds `rank`, `ranking`, and
`learning_to_rank`. It requires a group column, a whole-group validation split,
and features declared available at ranking time. A missing group, a row-level
split, or an after-outcome feature contract blocks deploy and run. The
distributed trainer separately verifies that a group is not spread across
participants.
