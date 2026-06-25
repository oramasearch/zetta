# Projects

An **AI project** (`zetta::Project`) is the central, user-managed artifact in
Orama Zetta. A project is instantiated from a **use-case type** (e.g. churn
intelligence) on a team's data, and — in later slices — owns that use case's
inputs, execution graph(s), and runs. Orama Zetta's job is to *manage projects
to success* (most AI projects fail), so the project is the root everything else
hangs off of. See [orama-zetta.md](orama-zetta.md) for that framing.

This page documents the **data-model root**: the `Project` type and its
lifecycle (CRUD) on a `Node`. Ingestion, graph compilation, and the New Project
screen are separate work.

## The type

A `Project` (`include/zetta/project.hpp`) is metadata only — it has no children
on disk yet:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `name` | `std::string` | Human name; unique within the node. |
| `created` | `std::string` | ISO-8601 UTC stamp at creation (e.g. `2026-06-25T14:03:21.123Z`). |
| `updated` | `std::string` | ISO-8601 UTC stamp; bumped on rename. |
| `owner` | `std::string` | Owner **key** (for key-scoped access); empty by default. |

Stamps come from `zetta::now_iso8601()` (`include/zetta/timestamp.hpp`) — a
fixed-width UTC string, so the values also order chronologically as plain text.

A project's references to its inputs / execution graphs / runs are **not** part
of this type yet; they attach in later slices (they will be id-only references,
keeping a project lightweight).

## CRUD on a Node

`Node` owns projects alongside databases, with the same shape as the database
CRUD (`shared_ptr` handles, empty/`false` on miss or clash):

| Method | Returns | Behavior |
|---|---|---|
| `create_project(name, owner = {})` | `shared_ptr<Project>` | Empty handle if `name` is empty or already taken. Stamps `created`/`updated`. |
| `find_project(name)` | `shared_ptr<Project>` | Empty handle on miss. |
| `drop_project(name)` | `bool` | `true` if one was removed. |
| `rename_project(from, to)` | `bool` | `false` if `to` is empty/taken or `from` is missing. Bumps `updated`. |
| `project_names()` | `vector<string>` | Sorted by name. |
| `project_count()` | `size_t` | — |

```cpp
auto churn = node.create_project("churn", "team:risk");   // owner key optional
node.rename_project("churn", "retention");                // keeps the id
node.drop_project("retention");
```

## Persistence & replication

Projects follow zetta's **"EVERYTHING is replicated state"** rule: the Automerge
CRDT document is the source of truth, and the in-memory list + on-disk folder are
a **projection** of it ([replication.md](replication.md)).

- **Replicated state.** Each project is one flat key in the document:
  `project:<uuid> → { name, created, updated, owner }` (`src/core/catalog.hpp`).
  Local create/rename/drop capture into the document via `ReplicationManager`;
  on each `tick()` the node **reconciles** the projection to match the document,
  so a peer's project (or rename/drop) materializes locally. Versions converge
  conflict-free, so a project created on one node appears on every peer.
- **On-disk projection.** A project is a folder named by its UUID under the data
  dir, holding `project.json` (`{ id, name, created, updated, owner }`):

  ```
  data_dir/
    <project-uuid>/
      project.json
  ```

  `Node::start()` rebuilds the in-memory list by scanning the data dir
  (`load_projects()`) and — when replicated — reconciling from the CRDT
  document, so a project **survives restart** in both standalone and replicated
  nodes. The folder name *is* the id, so rename only rewrites `project.json`.

`project.json` is **validated at the boundary** on load: a malformed or
incomplete file (not an object, or missing a string `name`) is skipped rather
than allowed to corrupt the in-memory tree — matching how `database.json` /
`table.json` are read.

## GUI panel

In headed mode the explorer's **leftmost column is Projects** — a thin renderer
over the `Node` project CRUD, mirroring the Databases column. It lets you:

- **create** a project (name field → `create_project`; inline error on an empty
  or duplicate name),
- **select** one (re-resolved by name each frame, like the database selection,
  so a dropped/renamed project never leaves a dangling handle),
- **rename** the selected project (`rename_project`; bumps `updated`), and
- **delete** one (`drop_project`, the `x` button).

Selecting a project shows its read-only details (short id, owner, `created`, and
`updated` when it differs). The panel holds no project state of its own — it
reads `project_names()` / `find_project()` every frame and writes through the
Node, so a project created/replicated elsewhere appears here, and no SDL/ImGui
type leaks into `zetta-core` (`src/gui/app.cpp` → `App::draw_projects`). This is
the lifecycle panel only; the full **New Project** screen (ingestion + use-case
selection, SCR-2 #223) is separate work.

## Tests

`tests/test_project.cpp` covers create/find/rename/drop (+ owner + stamps),
survival across a restart (both disk-only and CRDT-only), and replication of a
project + a rename to a connected peer.
