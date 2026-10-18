# Categorical features (one-hot encoding for the GBDT)

**Status:** shipped for the M-1 GBDT's table path (runs over a `database.table` and Study
trials) — [#1203](https://github.com/oramasearch/zetta/issues/1203), item 2 of
[#957](https://github.com/oramasearch/zetta/issues/957). Typing columns from the partition
catalog (item 3) is not part of this.

A churn table uploaded from a CSV carries `plan` and `region` as text. Those are the first
features most people expect a churn model to use, and until #1203 they were dropped with
`not used: 'plan' (not numeric; categorical columns are not supported yet)`. The GBDT reads
numeric columns only, so a low-cardinality text column is now **one-hot encoded**: one 0/1
feature per level, named `<column>=<level>` (`plan=basic`, `plan=pro`, …).

Code: `include/zetta/data/categorical_encoding.hpp` (the rules, fitting, row encoding), the
`categorical_features` field of `DatasetContract` (`dataset_path.hpp`), the indicator column
of `ddf::ColumnSchema::category_level`, and the model-side helpers in
`include/zetta/ml/score_table.hpp`.

## Which columns are categories

During contract inference ([dataset-path.md](dataset-path.md)) every remaining column that is
not numeric or boolean is tried as a category. It qualifies when, over the rows read:

| Rule | Otherwise, the `not used` note says |
|---|---|
| every non-null, non-empty value is text | `mixes text with non-text values` |
| it has at least one value | `no values in the sampled rows` |
| the values are not all dates or timestamps | `text dates or timestamps are not categories; derive a numeric age …` |
| 2 or more distinct values | `a single value ('x') carries no signal` |
| **at most 32 distinct values** (`kCategoricalMaxLevels`) | `text with N distinct values, above the 32-level cap for one-hot encoding: an identifier or free text, not a category` |
| at most half the values are distinct | `N of M values are distinct: the column identifies rows rather than grouping them` |
| no encoded name collides with a real column | `its encoded feature 'x=y' would collide with a column of the same name` |

The cap of 32 keeps a model's inputs legible and its trees from splitting on near-unique
values; a column above it is almost always an identifier, an email, a city list or free text.
To use one anyway, group it into fewer levels in a clean step.

The contract keeps categories in `categorical_features`, apart from the numeric
`feature_columns`, because only a trainer that encodes them may read them. A text column the
caller names in `feature_columns` stays there (a text methodology such as M-10 reads it from
there) and is also recorded as a category when it is one; the M-1 table path reads it as the
category.

## Fitted on the training rows, stored with the model

- **Fitting.** Inference proposes the levels from the sampled rows; `resolve_dataset` then
  refits them over every training row of the table — after the `as_of` cutoff when a time
  column is given — so a common level the sample missed is still a feature. A column that fails
  a rule over the full rows is dropped with a note. Levels are sorted; nulls and empty strings
  are not levels.
- **No leakage.** One-hot encoding reads no label, so its levels carry no target information
  into a held-out fold, and no target encoding is done. A level present only in the evaluation
  holdout produces an all-zero column on the training fold, which the trees cannot split on:
  the model treats it exactly like a level it never saw.
- **Stored.** The registered Model records the fitted encodings in
  `feature_schema.categorical_encoding`:

  ```json
  {"column": "region", "encoding": "one_hot",
   "levels": ["east", "north", "south", "west"],
   "features": ["region=east", "region=north", "region=south", "region=west"],
   "unseen": "a level not in `levels`, a null, or an empty string encodes as all zeros"}
  ```

  `feature_schema.features` lists the encoded names (what the trees split on), and
  `feature_schema.inputs` is **the predict input contract**: one entry per input column, in
  the model's order, `{name, type: "number"}` for a numeric feature and
  `{name, type: "string", encoding, levels, features, unseen}` for a category, named by its
  original column.

## Predict

Every predict path applies the model's **stored** encoding, never one refitted from the rows
it is given:

- `score` and `explain` with `rows` / `row`, and the served `POST /v1/models/{name}/predict`
  ([serving.md](../api/serving.md)): a row carries the original column and its text value
  (`{"usage": 50, "region": "south", "plan": "pro"}`).
- `score` and `explain` over a `dataset_path`: the table's original text column is read as one
  0/1 indicator per stored level.

An **unseen level**, a null or an empty string encodes as all zeros — the row is scored, not
refused. A row **without the column** is refused, naming it, exactly like a missing numeric
feature (zero-filling a missing column would score a value nobody measured). A number where
the model expects text is refused. A row that already carries the encoded features
(`region=south: 1`, …) instead of the column is accepted as it is.

## What the run says

- The run log notes each category the contract found, then what the trainer did with it:
  `categorical feature: 'region' one-hot encoded into region=east, region=north,
  region=south, region=west (4 levels fitted on the training rows and stored with the model;
  an unseen or missing value encodes as all zeros)`, or `not used: '<column>' (…)` with the
  reason.
- The run's feature report (`get_run_report.features`) lists each encoded feature under
  `used`, with `source: ["region"]` and a `transform` naming the levels.
- A methodology that reads numeric columns only (every one but M-1 today) excludes the
  category from its report and log with that reason.

## Proof

- `CategoricalFeatures.*` (`tests/data/test_dataset_path.cpp`): which columns qualify, the cap,
  identifiers and dates excluded, named text features, row encoding of seen/unseen/null/missing
  values, and the contract's JSON round trip.
- `GettingStartedChurn.TextCategoriesAreEncodedAndImproveTheHeldOutScore`
  (`tests/api/test_getting_started_churn.cpp`): an uploaded CSV whose signal is in `region` and
  `plan` trains through `run` with both encoded, a unique email column excluded, a held-out AUC
  of 0.85 against 0.46 with the numeric column alone, and `score`/`explain` by original names
  for a seen and an unseen level, a refused row without the column, and a table score.
- `GettingStartedChurn.CsvTrainsEndToEndAndStudyTrialUsesTheSameFeatures`: a run and a Study
  trial over the same table use the same encoded features.
- `ServingFixture.ServedPredictEncodesTextCategoriesWithTheStoredLevels`
  (`tests/api/test_serving.cpp`): the served route matches the reference scorer for a seen,
  an unseen and a null level.
