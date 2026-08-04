# `dataset_path` — one grammar, and what a table's columns mean

**Issue:** [#947](https://github.com/oramasearch/zetta/issues/947) ·
**Serves:** [#948](https://github.com/oramasearch/zetta/issues/948) ·
**Code:** [`include/zetta/data/dataset_path.hpp`](../../include/zetta/data/dataset_path.hpp),
[`src/core/data/dataset_path.cpp`](../../src/core/data/dataset_path.cpp) ·
**Tests:** `tests/data/test_dataset_path.cpp`

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
entity_column     the integer key of the thing being scored, one row per entity
label_column      the {0,1} training target — never a feature
time_column       optional point-in-time cutoff column (requires as_of)
as_of             only rows with time_column <= as_of contribute
feature_columns   the numeric columns to train on
```

Every field is optional. What is omitted is **inferred**, and every inference is
reported back in `notes` — a caller shows them, so nobody discovers an assumption
inside a trained model.

### The rules, in order

- **label** — the first of `churned` / `churn` / `is_churn` / `is_churned` /
  `did_churn` / `label` / `target` / `outcome` / `converted` / `y` that is present
  **and binary across the sampled rows**. Binary-ness outranks the name: a column
  called `label` holding free text is not a training target, and training on it would
  fail deep inside the GBDT rather than here, where the reason is still legible.
- **entity** — the first of `entity` / `entity_id` / `account_id` / `customer_id` / …
  / `id` that is present **and integer-valued**. Most specific first: a table carrying
  both `account_id` and `id` means the account, not the row.
- **features** — every remaining column whose sampled values are numeric, in catalog
  order, excluding the entity, label, and time columns. At least one is required: a
  label with no features is not a training set.
- **time / as_of** — **never inferred.**

Matching is case-insensitive (a warehouse round-trips `CHURNED` and `churned`
interchangeably) but the contract keeps the spelling the table actually carries.

### Why the cutoff is never inferred

A `time_column` and an `as_of` decide the **leakage boundary** — which rows a feature
may see. Guessing that would silently decide whether the model is honest. So a time
dimension applies only when the caller names it, and with no time column the rows are
taken as already point-in-time correct, which is the shape a connector query actually
produces (the windowing happened in the warehouse SQL).

### Why the entity key must be an integer

Entities are keyed by `std::int64_t` the whole way through the ML path
(`ml::EntityScore::entity`). A ranked at-risk list keyed by a number that joins back to
nothing is worse than a clear failure, so a string id column is reported as exactly
that, with the remedy (add an integer key column, or name one explicitly). A
whole-valued **double** is accepted: warehouses hand `NUMBER` columns back as doubles,
and refusing them would fail on the most common warehouse shape there is.

## Resolution

`resolve_dataset` resolves a Table-form path against the live catalog
(`Node::datasets()` — recomputed from the node tree joined with the partition catalog,
never a stored copy), samples it, and infers the contract. It fails, non-throwing and
with a path-qualified reason, when:

- the path is the Directory form (it says so rather than pretending);
- no such database/table exists — naming the near-misses, or what the cluster does have;
- the dataset is **degraded** ([#837](https://github.com/oramasearch/zetta/issues/837)):
  this node cannot see all of its partitions, so it would type — and then train on — a
  fraction of the rows while looking perfectly healthy;
- the table has no rows, or the contract cannot be inferred.

Reading rows for typing goes through the injected `DatasetSampler`, so the inference is
unit-testable against rows built in a test. The default reads through
`query::QueryService`, which routes a partition-only table through the
[scatter-gather select](scatter-gather-select.md).

## Boundaries

**No new JSON format.** `DatasetContract` is a value-type DTO with a hand-written
non-throwing `dataset_contract_from_json` mirroring `dataset_contract_schema()` — the
boundary rule for value types. Nothing here throws.

## Deliberately a later slice

- **Typing columns from the partition catalog.** The default sampler reads the whole
  table and keeps the first `kDefaultSampleRows` rows, because the SQL subset has no
  `LIMIT` yet. The catalog already records each body's column types
  ([#837](https://github.com/oramasearch/zetta/issues/837)), so typing from it — reading
  no rows at all — is the follow-up.
- **String entity keys.** Supporting them needs a derived integer column at frame-load
  time, plus a way to resolve keys back to business ids for the report. Until then the
  failure names the remedy.
