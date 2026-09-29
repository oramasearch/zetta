# Point-in-time snapshots and as-of joins

Issue [#1103](https://github.com/oramasearch/zetta/issues/1103), under the
[methodology epic](https://github.com/oramasearch/zetta/issues/984), adds the
native time-travel foundation used when runnable solution modules compute features from
time-ordered data. It has three parts:

1. `zetta::solutions::materialize_snapshot` persists one latest-visible row per
   entity at an inclusive cutoff.
2. `zetta::solutions::as_of_join` attaches the latest snapshot at or before each
   prediction row's own decision time.
3. `zetta::pipeline::SnapshotLineageCheck` refuses feature-bearing model graphs
   whose terminal sources are live tables, future snapshots, or overlapping
   feature/label windows.

The implementation is native C++26. It does not embed a foreign runtime or call
a hosted model service.

## Snapshot table contract

`include/zetta/solutions/snapshot.hpp` is the public interface. A
`SnapshotSpec` names an existing database and source table, a new or existing
target table, the entity/time columns, and `as_of`:

```json
{
  "database": "sales",
  "source_table": "account_events",
  "target_table": "account_features_2026_09_01",
  "entity_column": "account_id",
  "time_column": "observed_at",
  "as_of": "2026-09-01T00:00:00Z"
}
```

For every scalar entity key, materialization chooses the latest source row whose
time is `<= as_of`. It copies that row and writes four canonical lineage fields:

| Field | Meaning |
|---|---|
| `entity_id` | canonical entity key copied from `entity_column` |
| `as_of` | requested inclusive snapshot cutoff |
| `_source_as_of` | time of the selected source observation |
| `_source_table` | fully qualified source table name |

The logical key is `(entity_id, as_of)`. Retrying an identical materialization
is idempotent. A different row at an existing logical key is refused instead of
being overwritten. Numeric times and ISO-8601 UTC dates/date-times are accepted,
but one operation cannot mix the two representations.

Snapshot tables are ordinary Node-owned tables. `Database::create_table` and
`Table::add_row` put their metadata and rows through the existing catalog CRDT
capture/reconcile path, so peers receive them like every other table. Normal
source tables are read on the owner thread. Partition-only sources use the
existing one-owner-per-partition distributed select path.

## Capability and schemas

The `snapshot` action is registered as `orchestration:snapshot`. The single
registry entry produces both surfaces:

- MCP tool `snapshot`
- REST `POST /v1/snapshot`

The operation belongs to the existing `pipelines:manage` permission atom, so the
bootstrap owner and explicitly authorized pipeline operators can invoke it under
the always-on authorization gate.

Its input is `snapshot_spec.schema.json`; its result reports the target table,
source/materialized/existing row counts, excluded future rows, and the cutoff.
The schema index also publishes `as_of_join_spec.schema.json` for callers of the
C++ join interface. Both files are generated from the C++ accessors and checked
for drift with the rest of `schemas/`.

## Row-level as-of join

`AsOfJoinSpec` names entity and decision-time columns on the left and snapshot
key columns on the right. For each left row, `as_of_join` chooses the latest
right row for the same entity satisfying:

```text
right.as_of <= left.decision_time
```

An unmatched decision row is preserved without snapshot fields. Matched right
fields use the configurable `snapshot_` prefix by default. Duplicate right-side
`(entity, time)` keys are refused because choosing between them would make the
result depend on input order. The result reports matched, unmatched, and skipped
future candidates.

This row-level operator complements the DDF
[`as_of_aggregate`](../ddf/ddf-as-of.md): DDF reduces an event window into a
feature value, while this operator attaches an already materialized historical
row to a decision row.

## Correctness gate contract

The default gate includes `SnapshotLineageCheck` immediately after the existing
data-leakage check. It engages on a model step with a non-empty `features` array.
Such a model must declare:

```json
{
  "as_of": "2026-09-01T00:00:00Z",
  "feature_window_end": "2026-08-25T00:00:00Z",
  "label_window_start": "2026-09-01T00:00:00Z",
  "blackout_gap_seconds": 604800,
  "features": [{"name": "trailing_spend"}]
}
```

Every terminal data-flow ancestor must be a `Connect` step with:

```json
{
  "source_kind": "snapshot",
  "snapshot_as_of": "2026-09-01T00:00:00Z"
}
```

The check refuses the graph unless all of these invariants hold:

- each terminal source is a snapshot rather than a live table;
- each `snapshot_as_of` is at or before the model's prediction `as_of`;
- `feature_window_end` is at or before prediction time;
- `label_window_start >= feature_window_end + blackout_gap_seconds`.

The blackout is configured per model in seconds and must be non-negative. The
fields are part of `graph_step_annotations.schema.json`, so malformed values
fail at the graph boundary before the semantic comparisons run.

Methodology shape builders receive the same contract through explicit hints:
`snapshot_as_of` marks the Connect source, while `as_of`,
`feature_window_end`, `label_window_start`, and `blackout_gap_seconds` annotate
the feature-bearing model. The M-6 journey exercises that path by materializing
its input first; omitting any boundary leaves the compiled graph blocked. Its
runner carries the compiled `as_of` into featurization and uses only the
declared feature names, so snapshot lineage fields cannot become predictors.

## Portfolio integration

Time-ordered feature paths in runnable solution modules use this foundation in two
explicit phases: materialize or select a snapshot before feature construction,
then put the snapshot and window annotations above on the compiled graph. Static
dimension inputs do not require a synthetic time axis. The interface deliberately
does not depend on the registry, policy engine, holdout manager, or audit-ledger
foundation work; those components can call it through the public header.

## Refusal behavior

The implementation returns structured errors and never throws across the public
boundary. It refuses missing databases/tables, malformed rows or time values,
mixed time representations, conflicting snapshot keys, duplicate join keys,
live terminal feature sources, future snapshots, and violated blackout windows.
Tests cover the two acceptance cases directly: a live-table lineage is blocked,
and an as-of join never returns a row after the decision time.
