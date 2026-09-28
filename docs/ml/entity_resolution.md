# Entity resolution

Implemented for [#1041](https://github.com/oramasearch/zetta/issues/1041),
methodology M-2 with model kind `entity_resolution`.

## What it does

The native runner resolves records from several source systems into persistent,
deterministic entity identifiers. It emits a resolved-entity table mapping
`source, record_id` to `entity_id, confidence` and a match-pair table containing
the score, decision band, candidate routes, and deterministic owner partition.

Candidate generation combines three independent routes:

- normalized exact email and phone keys;
- sorted-neighbourhood windows over normalized names; and
- MinHash/LSH bands over name and address tokens.

Routes are deduplicated per pair. A stable seeded hash assigns every pair to one
owner partition so it is scored once. Fellegi–Sunter is the default scorer: EM
estimates match and non-match agreement probabilities from reducible sufficient
statistics. When labelled pairs can be derived from the optional truth entity
column, the runner trains the existing native histogram GBDT over pair features
and fits isotonic calibration on a deterministic held-out positive and negative
pair. When either class is too small for disjoint training and calibration, it
retains Fellegi–Sunter scoring.

Pair features are Jaro–Winkler name similarity, name and address token Jaccard,
normalized email and phone agreement, date distance, and separate missingness
indicators. A missing value therefore does not mean the same thing as a
disagreement.

## Inputs and outputs

The public API accepts either owned record partitions or a
`zetta::data::ResolvedDataset`. Table columns default to `source`, `record_id`,
`name`, `address`, `email`, `phone`, `date`, verification flags, and optional
`truth_entity`; every name is configurable in `EntityResolutionParams`.

`run_entity_resolution` reads the resolved table and writes both outputs under
`entity_resolution_results`. It reports excluded rows missing a source or record
identifier. It does not modify the input table.

## Scoring, clustering, and correctness

The stated defaults are match `>= 0.82`, review `>= 0.55`, and non-match below
`0.55`. Parameters must satisfy `0 <= review < match <= 1`.

Match edges are processed from strongest to weakest. A proposed union is refused
when it would combine incompatible verified emails or phones. When the dataset
declares at most one record per source, a union that duplicates a source is also
refused. Conflict checks inspect the whole candidate clusters before every union,
so a fuzzy bridge cannot create a transitive merge across a conflicting hard key.

## Metrics

With truth entities, the report includes candidate blocking recall, pairwise
precision/recall/F1, and B-cubed precision/recall. Every run also reports merge
rate, candidate-pair count, review-band size, the scoring method, and the stated
thresholds. Blocking recall is not presented when labels are absent.

## Distribution and determinism

The seed controls MinHash and pair ownership. Input records are sorted by their
source key before candidate generation, so partition and iteration order do not
change assignments. Fellegi–Sunter EM exposes an allreduce callback for its
sufficient statistics so a distributed caller can bind it to the compute
collective. The registry runner reads partitioned source rows through the query
service, then performs final EM and clustering on the coordinating member.
Clustering requires the globally ordered high-confidence edge set because every
transitive union must be checked against all verified identifiers.

## How to verify

```sh
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Entity*:*Resolution*:*Blocking*:*JaroWinkler*'
```

The focused tests plant duplicates with name and address typos, assert pairwise
F1 above 0.9 with B-cubed metrics, prove a verified-key bridge is not merged,
and compare one partition with three partitions.

## Running it

The registry integration compiles M-2 into its own connect, normalize/block,
score, cluster, and output pipeline. Run input names the pipeline table through
the connect step's `source` and `table`; an explicit `dataset_path` may override
that table. The run may also override its column contract and thresholds. Poll
the Run to completion, then query its two dataset outputs or call
`get_run_report` for the metrics, thresholds, source, exclusions, and output
table references.

## Boundaries and deferred work

No hosted model, embedded Python, foreign ML runtime, or new wire format is
introduced. Pair sufficient statistics use the existing collective vector
transport. Lake opportunity wiring is outside this change.
