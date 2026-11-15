# The import root: the one directory a caller may name files under

**Issues:** [#1391](https://github.com/oramasearch/zetta/issues/1391),
[#1433](https://github.com/oramasearch/zetta/issues/1433), the filesystem half of
[#1323](https://github.com/oramasearch/zetta/issues/1323) ·
**Code:** [`include/zetta/data/import_path.hpp`](../../include/zetta/data/import_path.hpp),
[`src/core/data/import_path.cpp`](../../src/core/data/import_path.cpp) ·
**Tests:** `tests/data/test_import_path.cpp`, `Uc1Journey.M1*DatasetDirectory*` in
`tests/api/test_action_journeys.cpp`

## What was wrong

Three capabilities took a path on the node's filesystem from the caller and opened it
with the node's own file access:

| Capability | Field | What it read |
|---|---|---|
| `add_dataset` | `path` | any parquet / jsonl / json / csv file, decoded into table rows |
| `run` (M-1) | `dataset_path`, directory form | `dataset.json`, `accounts.jsonl` and `events.jsonl` under any directory |
| `testsuite_run` | `custom_data_path` | the same fixed churn files under any directory |

So a caller holding `orchestration:add_dataset` and read access to the table it made could
import `<data_dir>/security/cluster-secret.json` and read the cluster join credential back
as a row. A run could probe any directory for the fixed churn file names, telling
a missing file from an invalid one, or train on a compatible dataset outside the catalog
(#1433). `parse_dataset_path` also asked the filesystem whether a bare name existed in
the node's working directory.

## The rule

Every path a caller names is **relative to `<data_dir>/imports`**, the node's import
directory. Operators stage files there (a volume mount, `kubectl cp`, a sidecar); the node
never creates or writes it. `zetta::data::resolve_import_path(node, requested, kind)`
decides, non-throwing:

1. **Lexical refusals, before the filesystem is touched**: an empty path, an absolute path
   (even one that points inside the import directory), a path starting with `~`, a
   backslash, a NUL byte, and any `..` component. A path outside the root is refused the
   same way whether it exists or not, so the refusal is not an existence oracle.
2. **The walk**: the import directory is opened, then each component with
   `openat(..., O_NOFOLLOW)` relative to the one before it. A symbolic link at any step,
   on the way or at the end, is refused, never followed. The last entry must be the kind
   asked for: a regular file (`add_dataset`) or a directory (`run`, `testsuite_run`).
3. **The real path check**: the canonical path of what was found must lie under the
   canonical import directory.

The import directory itself may sit behind a link (a mounted volume, macOS's
`/var → /private/var`); nothing below it may.

The node's security directory (`<data_dir>/security`: `cluster-secret.json`, the root
secret, the node certificate and key) and its identity files (`node_id`) are beside
`imports/`, never under it, so no accepted path reaches them.

`parse_dataset_path` no longer touches the filesystem: a bare name is always a table, and a
directory is written with a separator (`./churn`, `batches/2026-10`).

## Where it applies

| Caller | Resolved as | Refusal |
|---|---|---|
| `add_dataset` `path` | a file under the import root | `invalid_input`, `/path` |
| `run` M-1 `dataset_path` (directory form) | a directory under the import root | `invalid_input`, `/dataset_path` |
| `testsuite_run` `custom_data_path` | a directory under the import root | `invalid_input`, `/custom_data_path` |

`train_model`, `retrain_model`, `score`, `explain`, `analyze_dataset`, `plan_features` and
every other methodology already accepted only the `database.table` form. `register_model`
and `upload_dataset` take the file's bytes, not a path.

Trusted in-process callers still pass any path to `add_dataset_file` and
`load_churn_dataset`: the desktop app's file picker and drag-drop, the CLI, and tests. The
confinement is at the API boundary, where the path comes from a caller.

## Not covered here

- Paths a caller names for **writing** (`generate_dataset`'s `path`, `submit_feedback`'s
  `export_path`) go under the sibling export root, `<data_dir>/exports`, with the same
  lexical rules: [export-root.md](export-root.md)
  ([#1397](https://github.com/oramasearch/zetta/issues/1397),
  [#1399](https://github.com/oramasearch/zetta/issues/1399)).
- The resolver checks the path, then the loader opens it again by name. Someone who can
  already write inside the import directory could swap an entry between the two opens;
  the real-path check narrows that, and anyone with that access can stage the file anyway.
- Separating filesystem import authority from `orchestration:add_dataset` (the second
  recommendation of #1391) is a permission-vocabulary change, not made here: with the import root,
  that right reaches only what an operator staged.
