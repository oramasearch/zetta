# Entity segmentation

#1042 supplies the native `segmentation` trainer and table runner (Phase A), plus
the runnable M-3 methodology adapter (Phase B, stacked on the #1061 registry).
It groups entities by measured similarity and produces assignments, descriptive
profiles and a measured report through the existing REST/MCP control surface.

## Entry points and inputs

`train_segmentation` in `include/zetta/ml/segmentation.hpp` takes local entity rows,
`SegmentationParams`, and optional collective callbacks. `run_segmentation` takes a
`Node`, `data::ResolvedDataset`, parameters, and output options. Its participant
calls must run concurrently with the same feature contract and round tag, as for
GBDT's SPMD jobs. The registry adapter launches those participant calls through
the existing coordinated-launch listener.

Inputs contain one integer key per entity, finite numeric features, and optional
bounded categorical features declared in `categorical_columns`. Other strings,
including repeated free text, are excluded. `excluded_columns` declares additional
non-feature roles. The declared entity and target columns are excluded
before feature inspection. Identifier names are excluded as well. String columns
with excessive cardinality are excluded. Categorical levels have a global bound;
rare values share a collision-free additional indicator. Conventional id/key names
and `target`/`label`/`outcome` are excluded even when accidentally listed as
features in the native API. The graph gate rejects such explicit feature
requests. Table inference also excludes the shared resolver's known outcome
names (including `churned`, `converted` and `y`) without inspecting their values.
Non-finite numeric values or parameters fail explicitly. No target is required by the trainer.
Entity keys are checked for exact global uniqueness before training; this root
validation gathers keys only, not feature rows.
The shared `resolve_dataset` helper assumes a supervised contract. M-3's adapter
builds the same `ResolvedDataset` contract from catalog column names without
requiring a label or sampling label values. The trainer checks feature types and
category cardinality globally. Unspecified numeric features are inferred; only
`region`, `country`, `channel`, `plan`, `tier` and `category` names infer a
categorical role. Other categorical columns must be declared explicitly. The
report records those inferences and every exclusion.

## Running it

Land a table with `upload_dataset`, `add_dataset`, or an existing connector. For
example, `crm.customers` might contain integer `customer_id`, numeric `spend`,
`visits`, `tenure`, and categorical `region`. Then call:

```json
{"name":"customer-groups"}
```

with `create_project`, capture the intent with `capture_intent`:

```json
{"project":"customer-groups","text":"segment my customers into groups"}
```

and compile it with `compile_intent`:

```json
{
  "project":"customer-groups",
  "methodology":"M-3",
  "name":"customer segments",
  "hints":{
    "source":"crm",
    "table":"customers",
    "entity_column":"customer_id",
    "feature_columns":["spend","visits","tenure","region"],
    "categorical_columns":["region"],
    "seed":42
  }
}
```

`solution: S-9`, or the segmentation intent alone, selects the same methodology.
The default source is `datasets.customers`; set hints or edit the connect step
when the actual table differs. The five-step graph is connect → global feature
preparation → segmentation model → assignment and profile outputs. It has no
churn target, score index, retention action or demo fallback.

Call `run` with `{"project":"customer-groups","graph":"customer segments"}`.
The returned `engine` is `segmentation`, and the durable `run_id` is available
immediately. Poll `get_run` until `succeeded` or `failed`; failures name the
reason in logs. Read `get_run_report` with that `run_id` for K, its selection
reason, silhouette, Davies–Bouldin, stability, inertia and eligibility per K,
input table/read/clustered/excluded row counts, fitted features and excluded
columns, segment profiles, and both output table references. `limit` caps the
profile preview, not the assignment table. Output rows can also be read with SQL.

Optional `run.dataset_path` explicitly overrides the graph table; `run.dataset`
overrides its entity/label/time/feature roles and is checked again. Only landed
tables are supported. No missing source can silently select demonstration data.
Supervised `holdout_fraction` and ranked `top_k` run options are refused for M-3.
The config schema is discoverable as `segmentation_config` through `get_spec`.
It bounds resource knobs (K up to 64, 2..32 restarts, a sample up to 8,192 rows),
requires the stated global scaling/encoding/selection, and refuses a minimum
share below 2%. Time filters require an explicit `time_column` and `as_of`;
neither is guessed. Both the cutoff and column values accept ISO-8601 timestamps
or finite epoch seconds and are normalized to epoch seconds before the inclusive
comparison. Rows after the cutoff never reach feature preparation or fitting.

`SegmentationCheck` runs through the default `Check` interface for gate/deploy/run.
It refuses entity/label/outcome features, local scaling, unbounded encoding,
different preparation/model contracts, an alternative K criterion, weakened
minimum share, malformed parameters, and incomplete or disconnected outputs.
Runtime checks additionally enforce global key uniqueness, finite values,
partition completeness and the selected segment shares.

## Training and selection

Numeric scaling is fitted on the rows being clustered, using global statistics.
Ordinary features use mean and standard deviation; heavy tails use median and
interquartile range. Quartiles use exact interpolated global ranks through count
allreduces, not bins whose precision degrades in the presence of outliers.
Categorical values are one-hot encoded. The fitted transform
travels with the model and applies unchanged to the evaluation sample.

Parallel k-means initialization selects candidate centers over local rows, assigns
global population weights, and reduces them with weighted k-means++. Lloyd
iterations combine local counts and coordinate sums with `JobRound::allreduce`.
Distances fan out across CPU workers; sums retain input order. Empty clusters are
handled deterministically; a solution with any remaining empty cluster is not
eligible. Constant data cannot silently publish non-finite diagnostics.
Iteration stops at the configured
center-shift tolerance or maximum iteration count. Defaults are k=2..8, five
restarts, 100 iterations, tolerance 0.0001, and minimum segment share 0.02.

The chosen k maximizes silhouette on a deterministic sample of up to 2,048 rows,
breaking ties by lower Davies–Bouldin and then smaller k. Candidates below the
minimum share are excluded, allowing a smaller k to absorb tiny groups; k=2 is
the documented exception. Stability is the mean pairwise adjusted Rand index
between restart assignments on the same sample. Inertia is recorded for each k.
These are descriptive clustering diagnostics, not held-out predictive accuracy.

## Outputs and lineage

The designated writer writes a `Model` with kind `segmentation`, its fitted
transform and centers, and numeric metrics. The selection reason lives in
`feature_schema.chosen_k_reason`. The runner creates tables in
`segmentation_results` and records their references on a `Run`:

| Output | Columns |
|---|---|
| Assignments | `entity`, `segment_id`, `distance_to_center` in transformed space |
| Profiles | `segment_id`, `size`, `share`, `center`, `distinguishing_features`, `label` |

Numeric profile centers are in original units; categorical centers name a dominant
level. Distinguishing features rank deviations from the overall mean in transformed
units. Labels such as `high failed_payments, low tenure` describe differences,
never causes. Run lineage names the input dataset and computation stages.

The trainer is native C++26 without a foreign ML library or hosted model. Numeric
partials use the existing validated `CollectiveMessage` transport. Candidate and
sample coordination use the root; training rows remain on their owning members.
Final assignment materialization is single-member: assignments are gathered and,
for registry runs, broadcast to the initiating writer, which may be off-root.
Model metadata and Run records use their existing replicated boundaries. The
weights JSON remains an internal write-only artifact. Config and peer-launch
payloads have generated, validated schemas; the existing report capability
declares its additional segmentation fields.

The runner checks the partition manifest and collective input row count. Exactly
one designated owner (lowest owner id) contributes each replicated partition;
an unavailable designated owner fails explicitly, without substituting a local
table copy. Unpartitioned replicated tables are read only on the root. An explicit
time cutoff is applied before fitting. Entity keys retain their full signed
64-bit precision. Node access and output writes go through the owner-thread gate,
and writer persistence success is allreduced before peers report success. Native
calls default to a root-created Run; registry calls reuse the Run returned at
submission. A roster change or missing peer fails with a bounded timeout. The
peer launch is at-most-once, like the existing coordinated-launch mechanism.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Segment*:*KMeans*:*Cluster*'
./build/tests/zetta-tests --gtest_filter='*Segment*:*KMeans*:*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
./build/tests/zetta-tests
```

`tests/ml/test_segmentation.cpp` covers planted-group recovery, choosing k=3,
cross-member collective equivalence and three stored owned partitions, target and identifier exclusion, minimum
share, categorical encoding, descriptive profiles, and persisted table/Model/Run
outputs. Same seed and partitioning must produce identical results. Across
different partitionings, floating-point sums are compared within numeric tolerance.

## Deferred

Lake acceptance and GUI-specific report rendering are outside this work order.
GMM, HDBSCAN, surrogate models,
downstream feature serving, and device-native distance kernels are outside this
k-means slice.

Phase B adds a report output reference alongside the model and dataset references
and regenerates `schemas/openapi.json`. The registry discovery/compile/run journey
and graph refusal tests live in `tests/api/test_action_journeys.cpp` and
`tests/ml/test_segmentation_methodology.cpp`.
`SegmentationMethodology.OffRootSubmissionLaunchesThreeOwnedPartitions` exercises
automatic peer launch over three real partitioned members, verifies the global
result against the single-node oracle, and submits from a non-root writer.
