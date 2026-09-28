# Entity segmentation

Phase A of #1042 adds the native `segmentation` trainer and table runner. It
groups entities by measured similarity and produces assignments and descriptive
profiles. Registry, intent compilation, REST/MCP, and report rendering are Phase B
after #1061; this page does not claim those integrations are available.

## Entry points and inputs

`train_segmentation` in `include/zetta/ml/segmentation.hpp` takes local entity rows,
`SegmentationParams`, and optional collective callbacks. `run_segmentation` takes a
`Node`, `data::ResolvedDataset`, parameters, and output options. Its participant
calls must run concurrently with the same feature contract and round tag, as for
GBDT's SPMD jobs. Automatic peer launch belongs to the later adapter.

Inputs contain one integer key per entity, finite numeric features, and optional
bounded categorical features declared in `categorical_columns`. Other strings,
including repeated free text, are excluded. `excluded_columns` declares additional
non-feature roles. The declared entity and target columns are excluded
before feature inspection. Identifier names are excluded as well. String columns
with excessive cardinality are excluded. Categorical levels have a global bound;
rare values share a collision-free additional indicator. Conventional id/key names
and `target`/`label` are excluded even when accidentally listed as features.
Non-finite numeric values or parameters fail explicitly. No target is required by the trainer.
Entity keys are checked for exact global uniqueness before training; this root
validation gathers keys only, not feature rows.
The existing `resolve_dataset` helper still assumes a supervised contract; callers
with unlabeled tables must supply an explicit `ResolvedDataset` until the shared
resolver supports unsupervised methodologies.

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

The deterministic root writes a `Model` with kind `segmentation`, its fitted
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
sample coordination and final output materialization use the root; training rows
remain on their owning members. Model metadata and Run records use their existing
replicated boundaries. The weights JSON is an internal write-only artifact in
Phase A; there is no new public JSON parser or capability schema.

The runner checks the partition manifest and collective input row count. Exactly
one designated owner (lowest owner id) contributes each replicated partition;
an unavailable designated owner fails explicitly, without substituting a local
table copy. Unpartitioned replicated tables are read only on the root. An explicit
time cutoff is applied before fitting. Entity keys retain their full signed
64-bit precision. Node access and output writes go through the owner-thread gate,
and root persistence success is broadcast before peers report success. Only the
root returns persisted artifact ids in this slice.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Segment*:*KMeans*:*Cluster*'
./build/tests/zetta-tests
```

`tests/ml/test_segmentation.cpp` covers planted-group recovery, choosing k=3,
cross-member collective equivalence and three stored owned partitions, target and identifier exclusion, minimum
share, categorical encoding, descriptive profiles, and persisted table/Model/Run
outputs. Same seed and partitioning must produce identical results. Across
different partitionings, floating-point sums are compared within numeric tolerance.

## Deferred

Phase B supplies the methodology entry, automatic peer launch, pipeline shape,
graph correctness checks, `get_run_report`, agent/control-surface integration,
OpenAPI regeneration, and the lake acceptance. GMM, HDBSCAN, surrogate models,
downstream feature serving, and device-native distance kernels are outside this
k-means slice.

No capability changed in Phase A, so `schemas/openapi.json` is unchanged. The
runner emits dataset and model output references; a report reference will be
added only when Phase B provides the corresponding renderer.
