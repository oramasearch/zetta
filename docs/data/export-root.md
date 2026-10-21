# The export root: the one directory a caller may have the node write in

**Issues:** [#1397](https://github.com/oramasearch/zetta/issues/1397) (LSR-164),
[#1399](https://github.com/oramasearch/zetta/issues/1399) (LSR-166) ·
**Code:** [`include/zetta/data/export_path.hpp`](../../include/zetta/data/export_path.hpp),
[`src/core/data/export_path.cpp`](../../src/core/data/export_path.cpp) ·
**Tests:** `tests/data/test_export_path.cpp` · **Sibling:** [import-root.md](import-root.md)

## What was wrong

Two capabilities wrote a file on the node at a path the caller chose, with a truncating
open and the node's own file access:

| Capability | Field | Who could call it | What it wrote |
|---|---|---|---|
| `generate_dataset` | `path` | `orchestration:run` (in `pipelines:manage`); Lake forwards it from a workspace writer | the generated JSONL corpus |
| `submit_feedback` | `export_path` | `api:query`, which the built-in reader holds | the feedback bundle (JSON) |

So a caller could truncate any file the engine's OS account can write: the security files
under `<data_dir>/security` (a node whose security JSON no longer parses does not start
again), catalog state, configuration. The bytes were constrained (corpus or bundle JSON),
but the damage to the node was persistent.

## The rule

A path a caller names for writing is **relative to `<data_dir>/exports`**, the node's export
directory. `zetta::data::write_export_file(node, requested, bytes)`, non-throwing:

1. **Lexical refusals, before the filesystem is touched**, shared with the import root
   (`caller_path_components`): an empty path, an absolute path (even one inside the export
   directory), a leading `~`, a backslash, a NUL byte, any `..` component, and the root
   itself (`.`).
2. **The root**: the data directory is opened (it may sit behind a link, a mounted volume),
   then `exports` inside it is created if missing (mode `0700`) and opened with
   `O_NOFOLLOW | O_DIRECTORY`: the export directory itself may not be a link.
3. **The walk**: each directory component is created if missing (`mkdirat`, mode `0700`)
   and opened with `openat(..., O_NOFOLLOW | O_DIRECTORY)` relative to the one before it, so
   a symbolic link on the way is refused, never followed.
4. **The file** is created with `openat(..., O_CREAT | O_EXCL | O_NOFOLLOW, 0600)`. An
   existing file, an existing link (dangling or not) and an earlier export are all refused:
   **an export never overwrites anything**. A write that fails removes the partial file.

The node's security directory and identity files sit beside `exports/`, never under it, so
no accepted name reaches them.

## Where it applies

| Capability | Field | Output | Refusal |
|---|---|---|---|
| `generate_dataset` | `path` | `path`: the written file, relative to the export root | `invalid_input`, `/path` |
| `submit_feedback` | `export_path` | `exported_to`: the written file, relative to the export root | `invalid_input`, `/export_path` (the bundle is still stored as a large object; the message names its id) |

Both fields now have `minLength: 1`. Lake's Studio never sends `path` to `generate_dataset`
(its data page says so), and nothing in this repository passed either field.

Operators read exports from the node's volume (`kubectl cp`, a sidecar, a mounted volume).
Trusted in-process callers (the desktop app, the CLI, tests) still write where they like:
the confinement is at the API boundary, where the name comes from a caller.

## Not covered here

- `submit_feedback` still attaches the node's most recent traces, errors and crash reports
  regardless of who asks, and needs only `api:query` (LSR-166's second and third
  recommendations). Filtering the attachments by the caller's authority and requiring an
  export right are tracked separately (see #1399's closing comment).
- Disk use: each export is a new file and nothing prunes `exports/`. A caller with the right
  can fill the volume, as it can already through the large-object store.
