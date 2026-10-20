# `dataset_path` — one grammar, and what a table's columns mean

**Issue:** [#947](https://github.com/oramasearch/zetta/issues/947) ·
**Serves:** [#948](https://github.com/oramasearch/zetta/issues/948) ·
**Code:** [`include/zetta/data/dataset_path.hpp`](../../include/zetta/data/dataset_path.hpp),
[`src/core/data/dataset_path.cpp`](../../src/core/data/dataset_path.cpp) ·
**Tests:** `tests/data/test_dataset_path.cpp`, `tests/data/test_dataset_catalog_typing.cpp` (#1203 item 3), and the #956 timing in `tests/usecases/churn/test_motive_demo_dataset.cpp`

## The break this closes

Two things were both called a dataset, and nothing converted one into the other:

- the **connector/ingest path** (`add_dataset`, `create_dataset_from_query`) lands rows
  as parquet partitions placed across the cluster, addressed `database.table` and
  surfaced in [`system.datasets`](system-datasets.md);
- the **run path** wanted a filesystem directory holding a `dataset.json` manifest plus
  its row files.

So the data a customer actually brings — through a connection, through a query — could
not reach the thing that trains a model on it. `dataset_path` accepted only the second
form: `dataset_path = "churn_demo.accounts"` failed with `cannot open dataset.json`
while the rows sat there, partitioned and healthy.

## The grammar

`parse_dataset_path` accepts both forms and never guesses between them:

| Input | Kind | Why |
|---|---|---|
| `/var/lib/churn`, `./data/churn.v2`, `data/churn`, `~/fixtures/churn` | **Directory** | contains a separator, or starts with `.` / `~` |
| an existing filesystem entry | **Directory** | the existence test only ever moves a path *into* Directory, so it can never steal a live table |
| `churn_demo.accounts` | **Table** | `database.table` |
| `prod.eu.accounts` | **Table** | the **last** dot separates, so a dotted database name still resolves |
| `accounts` | **Table** | bare name, in the default database |

The separator test comes **first** because a path is the unambiguous form: a directory
may legitimately contain a dot, while a table name may not contain a slash. `~` is not
expanded here — expansion belongs to the caller that owns the filesystem, not to a
grammar.

## The contract — what the columns mean

A dataset directory says what its columns mean in its manifest. A partitioned table
does not: parquet carries names and physical types, never which column is the label.
`DatasetContract` is that missing statement.

```
entity_column     the integer or text business key, one row per entity
label_column      the numeric or boolean target (inferred only for {0,1}) — never a feature
time_column       optional point-in-time cutoff column (requires as_of)
as_of             ISO-8601 timestamp or epoch seconds; only earlier rows contribute
feature_columns   the numeric or boolean columns to train on
categorical_features  low-cardinality text columns and their fitted levels (resolved, not given)
```

When the time role is present, `as_of` and every value in `time_column` may use
either ISO-8601 text or finite epoch seconds. Resolution converts both forms to
epoch seconds before comparison. A dataset must use one representation
consistently, and a row strictly after the inclusive cutoff is excluded.

Every field is optional. What is omitted is **inferred**, and every inference is
reported back in `notes` — a caller shows them, so nobody discovers an assumption
inside a trained model.

### The rules, in order

- **label** — the first of `churned` / `churn` / `is_churn` / `is_churned` /
  `did_churn` / `label` / `target` / `outcome` / `converted` / `y` that is present
  **and binary** (a boolean in the partition catalog, or 0/1 across the sampled rows). Binary-ness outranks the name: a column
  called `label` holding free text is not a training target, and training on it would
  fail deep inside the GBDT rather than here, where the reason is still legible.
- **entity** — the first of `entity` / `entity_id` / `account_id` / `customer_id` / …
  / `id` that is present and integer-valued or text. Most specific first: a table carrying
  both `account_id` and `id` means the account, not the row.
- **features** — every remaining column that is numeric or boolean (by its partition catalog
  type, or across the sampled rows when the catalog cannot type the table), in catalog
  order, excluding the entity, label, and time columns. A remaining **text** column with 2 to
  32 levels that does not identify rows and is not a date goes to `categorical_features`
  instead, and `resolve_dataset` refits its levels on every training row (after the cutoff);
  any other column is left out with a `not used: '<column>' (…)` note saying why. At least one
  numeric or categorical feature is required: a label with no features is not a training set.
  A text column named in `feature_columns` stays there (a text methodology reads it) and is
  also recorded as a category when it is one. Only the M-1 GBDT's table path (runs and Study
  trials) one-hot encodes categories; every other methodology reads numeric columns and says
  so. The rules, the stored encoding and predict are in
  [categorical-features.md](categorical-features.md) (#1203).
- **time / as_of** — **never inferred.**

Matching is case-insensitive (a warehouse round-trips `CHURNED` and `churned`
interchangeably) but the contract keeps the spelling the table actually carries.

### Why the cutoff is never inferred

A `time_column` and an `as_of` decide the **leakage boundary** — which rows a feature
may see. Guessing that would silently decide whether the model is honest. So a time
dimension applies only when the caller names it, and with no time column the rows are
taken as already point-in-time correct, which is the shape a connector query actually
produces (the windowing happened in the warehouse SQL).

### Text entity keys

Numeric algorithms remain keyed by `std::int64_t`. For a text entity column, contract
resolution hashes each distinct value deterministically into the exact-integer range used
by native grouping and refuses a collision within the dataset. The resolved contract keeps
both directions of the mapping. Frame
loading supplies the derived key to numeric operators; score tables, rankings, segment
assignments, methodology outputs, decisions, action deliveries and reports restore the
original business value. Runs and Models persist the mapping for audit and later scoring.
Integer keys continue unchanged and carry no mapping. A whole-valued **double** is also
accepted because warehouses commonly return `NUMBER` columns as doubles.

M-8 deterministically compacts derived keys to dense ALS indices while training, then
restores the derived keys before applying the dataset mapping. The recommendation table
and downstream policy therefore still expose only the original business value.

The contract note is explicit: `entity column 'customer_id' (text; keyed by a derived
id, outputs carry the original value)`.

### Boolean labels and features

JSON booleans in a label or feature column load as `false = 0` and `true = 1`. Any
other non-numeric label is refused during contract resolution, before a Run starts, with
the column name and a cast remedy such as `churned::int`. Methodologies that declare
text or categorical feature roles retain their existing methodology-specific validation.
Explicit continuous numeric targets remain valid; only inferred labels must be binary.

## Resolution

`sample_dataset` resolves a Table-form path against the live catalog
(`Node::datasets()` — recomputed from the node tree joined with the partition catalog,
never a stored copy) and samples it. `resolve_dataset` builds the training contract
from that shared result; [`analyze_dataset`](featurization.md) builds semantic and
table-shape evidence from it. Both fail, non-throwing and with a path-qualified reason,
when:

- the path is the Directory form (it says so rather than pretending);
- no such database/table exists — naming the near-misses, or what the cluster does have;
- the dataset is **degraded** ([#837](https://github.com/oramasearch/zetta/issues/837)):
  this node cannot see all of its partitions, so it would type — and then train on — a
  fraction of the rows while looking perfectly healthy;
- the table has no rows, or the contract cannot be inferred.

Reading rows goes through the injected `DatasetSampler`, so contract and semantic
inference are unit-testable against rows built in a test. The default reads through
`query::QueryService`, which routes a partition-only table through the
[scatter-gather select](scatter-gather-select.md). `sample_dataset` (used by
`analyze_dataset`) still reads whole rows; `resolve_dataset` types the contract from the
partition catalog first, below.

## Typing from the partition catalog

[#1203](https://github.com/oramasearch/zetta/issues/1203) item 3. Every partition body
records its own schema in the catalog
([#837](https://github.com/oramasearch/zetta/issues/837)): the Parquet physical type of each
column, or the JSON family for a JSON Lines body. `catalog_column_types` merges those over
the table's partitions into one `CatalogColumnType` per column:

| Recorded | Type | What it decides without a row |
|---|---|---|
| `boolean` | `Boolean` | numeric, binary, integer: a feature, a label, an entity key |
| `int32`, `int64`, `integer` | `Integer` | numeric and integer: a feature or an entity key. Not whether it is 0/1 |
| `float`, `double`, `number` | `Number` | numeric: a feature. Not whether it is 0/1, nor whether it is whole |
| `null` (JSON Lines) | `Null` | no values: `not used` |
| `byte_array`, `string`, `mixed`, anything else | `Undecided` | nothing. Text, a mixed column, or an all-null Parquet column (written as `byte_array`) |

Partitions merge as you would expect: an integer and a real partition make `Number`, a boolean
and an integer partition make `Integer`, `Null` gives way to the other side, and `Undecided`
wins. The catalog types the table only when **every row** is in a partition that recorded its
schema: a table that was never partitioned, a partition written before #837, or a catalog that
covers fewer rows than the table holds (rows inserted after the ingest) leaves it unavailable,
and the table is typed from sampled rows exactly as before.

**What still reads rows, and why.** `columns_needing_values` names the columns whose values
decide something the catalog does not record; only those are read:

- **Every `Undecided` column, from the first `kDefaultSampleRows` rows.** Text has to be
  looked at to tell a category (2 to 32 levels, not an identifier, not a date —
  [categorical-features.md](categorical-features.md)) from an identifier, a date or free text,
  and a text entity has to be checked for empty values. This is the read that keeps item 2's
  decisions what they were: the same rows, the same rules.
- **A label candidate typed `Integer` or `Number`** (when no label is named), from the same
  rows: inference picks the first candidate whose values are all 0/1, and the catalog does not
  record that. A boolean candidate needs no read.
- **An entity candidate, or the named entity, typed `Number`**: a real column is an entity key
  only when every value is whole.
- **Fitting, over every row** — only when there is a text entity or a category: the entity
  column (its derived keys), each category column (its levels, refitted on the training rows),
  and the time column that picks the training rows. Nothing else.

A table of numbers with a boolean label, or with the entity and label named, is typed with
**no row read at all**. The first note says which happened:

```
column types: from the partition catalog (2 partitions); values of churned, finance_segment, finance_sub_segment, sfdc_account_id read from the first 256 rows (text, or a label or entity the catalog cannot type)
column types: from the partition catalog (4 partitions); no rows read to type them
column types: from the first 256 rows, every column (the table has no partitions in the catalog)
```

`ResolvedDataset::reads` carries the same facts as data: whether the catalog typed it (and
why not), the sampled columns and row count, and the fitted columns and row count.

The reads are projected: `read_dataset_columns` runs `SELECT <columns> FROM <db>.<table>`
rather than `SELECT *`. A column name the SQL subset cannot write as a bare identifier (it has
no quoted identifiers: `plan tier`, `limit`) falls back to `SELECT *` and is projected in
memory, so the answer is the same. With an injected sampler, the sampler's rows are used and
cut to the same columns.

**One deliberate difference from the sample.** A column the catalog types numeric whose
first 256 rows happen to be all null is a feature: the catalog has seen its values, where the
sample alone left it out with `no values in the sampled rows`. Every other decision is the
sample's, and `CatalogTyping.TypesMatchWhatTheSamplerProduced` holds it to that by resolving
the same rows landed partitioned, unpartitioned and partition-only.

**Measured** ([#956](https://github.com/oramasearch/zetta/issues/956)'s path:
`MotiveDemoDataset.TrainsAndExplainsWithTreeShap` over the 15,000-row demo table from
`scripts/gen_motive_demo_dataset.py --accounts 15000`, median of 5 per run, three runs each,
macOS arm64 Release, same machine back to back):

| | before | after |
|---|---|---|
| `resolve_dataset`, entity and label named | 86–87 ms | 34–36 ms |
| `resolve_dataset`, label inferred | 86–88 ms | 37–39 ms |
| `score_table` (15,000 rows, unchanged) | 65–71 ms | 65–70 ms |

Before, a resolve ran `SELECT *` twice (the typing sample and the fit for the two
categories). After, it reads four of eighteen columns for typing (`churned` when the label is
inferred, `finance_segment`, `finance_sub_segment`, `sfdc_account_id`) and two for fitting.

**With SQL `LIMIT` pushdown** ([#1021](https://github.com/oramasearch/zetta/issues/1021)): the
typing read already sends `LIMIT <n>` (`SELECT <columns> FROM <db>.<table> LIMIT 256`, and the
whole-row fallback `SELECT * … LIMIT 256`). Without the pushdown the query service still scans
the table and cuts the result to the same first 256 rows; with it, the scan stops at the limit
and the typing read costs 256 rows. Nothing on this path changes when the pushdown lands. The
fitting read reads every row by design.

## Boundaries

`DatasetContract` is a value-type DTO with a hand-written
non-throwing `dataset_contract_from_json` mirroring `dataset_contract_schema()` — the
boundary rule for value types. Its optional `entity_key_mapping` carries the derived id,
original value pairs for a text entity column. Nothing here throws.

## Running on the resolved table

`zetta::usecases::churn::run_churn_solution_on_table` takes the resolved table id plus
its contract and runs a churn graph over it **compute-near-data**: the accounts shard
comes from this node's own partition bodies (`ddf::load_local_partitions`) rather than
from an injected store, so the rows never cross the wire. That is the difference from
the in-memory path, which encodes the whole dataset on the initiator and ships it to
every peer.

The featurization runs in **static carry-through mode** — no windowed event features.
A connector query returns one already-featurized row per entity, so there are no events
to aggregate, and inventing windowed aggregates over rows that are not events would be
arithmetic on a shape that does not exist. When the contract names a time column the row
universe is still cut at `as_of`, so the point-in-time guarantee holds where it applies.

The entity is renamed to the canonical `entity` on the way into the frame (via the column
schema's `field`). For text ids that numeric column contains the derived key internally;
every persisted or delivered entity field resolves it back to the original value.

## Over the API

`run` and `agent_run` take `dataset_path` in either form, plus an optional `dataset`
object carrying the contract overrides (validated against `dataset_contract_schema()`
at the boundary). A table run reports back what it **inferred**:

```json
{ "run_id": "...", "engine": "churn", "status": "running",
  "dataset": { "entity_column": "account_id", "label_column": "churned",
               "feature_columns": ["failed_payments", "logins_30d", "nps"] },
  "dataset_notes": ["column types: from the partition catalog (8 partitions); values of churned read from the first 256 rows (text, or a label or entity the catalog cannot type)",
                    "label column: 'churned' (binary across the sampled rows)",
                    "entity column: 'account_id' (integer by its type in the partition catalog, integer)",
                    "feature columns (3): failed_payments, logins_30d, nps",
                    "no time column given — the rows are taken as already point-in-time correct"] }
```

The notes are not decoration. An assumption the caller never sees is one they discover
inside a trained model.

## Deliberately a later slice

- **Multi-node co-launch for the table path.** The SPMD rounds need every participant
  co-running; the in-memory path fans a launch out over the signal plane, and the table
  path's fan-out is not wired yet. With peers on the roster the run **fails with the
  reason** rather than training one node's shard and reporting it as the whole. The
  payload is small — a table id and a contract, not the rows — so this is a smaller
  change than the one it replaces.

- **A `LIMIT` that bounds the typing read.** Typing reads only the columns the catalog
  cannot decide (above) and sends `LIMIT kDefaultSampleRows`, but the scan still visits
  every row until SQL `LIMIT` reaches the scan
  ([#1021](https://github.com/oramasearch/zetta/issues/1021)).
