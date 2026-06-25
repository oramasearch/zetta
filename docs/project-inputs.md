# Project inputs

A **project input** (`zetta::ProjectInput`) is a raw input captured into a
[project](projects.md) with **provenance**. It is the first slice of ingestion
(FEAT-1): a team starts a project either by describing what they want in natural
language (**intent**) or by handing over something they already have
(**artifact** — R1's first artifact type is an n8n workflow JSON; SQL /
notebooks / schemas planned). zetta stores those raw inputs so the project can be
(re-)compiled from them later.

This page documents the **record + provenance** only — *storage, not parsing*.
Nothing here interprets, validates, or compiles a payload; that is a later
FEAT-1 slice. A `ProjectInput` is owned by a `Project` exactly as a `Table` is
owned by a `Database`.

## The type

A `ProjectInput` (`include/zetta/project_input.hpp`) is provenance metadata plus
a raw payload body:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `project_id` | `std::string` | The owning project's id (the CRDT parent ref). |
| `kind` | `ProjectInputKind` | `Intent` or `Artifact`. |
| `source` | `ProjectInputSource` | `Upload` (a file) or `Pasted` (typed/pasted text). |
| `filename` | `std::string` | Original filename; empty for pasted text. |
| `content_hash` | `std::string` | Lowercase-hex SHA-256 of the payload (computed at the boundary). |
| `size` | `std::uintmax_t` | Payload size in bytes. |
| `created_by` | `std::string` | Actor/user key that created the input; empty by default. |
| `created_at` | `std::string` | ISO-8601 UTC stamp at creation. |

`kind` and `source` have canonical lowercase names (`intent`/`artifact`,
`upload`/`pasted`) via `to_string` / `project_input_kind_from` /
`project_input_source_from`. Those strings are **both** the display label **and**
the stored representation (what's written to the CRDT value and `input.json`), so
they are stable on-disk identifiers. The `created_at` stamp comes from
`zetta::now_iso8601()` — fixed-width, so inputs also order chronologically as
plain text.

An input is an **immutable provenance record**: it is created once and never
renamed or edited (only created or dropped).

## Creating an input

Inputs are created through the owning `Project`:

```cpp
auto project = node.create_project("churn");

// Natural-language intent, pasted (no filename):
project->create_input(zetta::ProjectInputKind::Intent,
                      zetta::ProjectInputSource::Pasted,
                      "score every active account for churn risk and tell me why",
                      /*filename=*/{}, /*created_by=*/"user:steve");

// An artifact the team uploaded (keeps its original filename):
project->create_input(zetta::ProjectInputKind::Artifact,
                      zetta::ProjectInputSource::Upload,
                      workflow_json, "churn.json", "user:charles");
```

`create_input` takes the **raw payload content**, computes
`content_hash = sha256(payload)` itself (it is never trusted from the caller —
boundary integrity), stamps `created_at`, writes the metadata into the CRDT and
the payload body to disk, and returns a `shared_ptr<ProjectInput>` handle.

| Method (on `Project`) | Returns | Behavior |
|---|---|---|
| `create_input(kind, source, payload, filename = {}, created_by = {})` | `shared_ptr<ProjectInput>` | Captures + persists; empty handle only if the folder can't be created. |
| `find_input(id)` | `shared_ptr<ProjectInput>` | Empty handle on miss (lookups are by id). |
| `drop_input(id)` | `bool` | `true` if one was removed (deletes folder + CRDT key). |
| `inputs()` / `input_count()` | list / `size_t` | Inputs sorted oldest-first by `created_at`. |

Read the payload back with `input->read_payload()` (`std::optional<std::string>`;
`std::nullopt` if the body isn't present locally — see below).

## Persistence & replication

Project inputs follow zetta's **"EVERYTHING is replicated state"** rule: the
Automerge CRDT document is the source of truth and the in-memory list + on-disk
folder are a **projection** of it ([replication.md](replication.md)).

- **Replicated metadata.** Each input is one flat key in the document:
  `input:<uuid> → { project, kind, source, filename, content_hash, size,
  created_by, created_at }` (`src/core/catalog.hpp`). Local create/drop capture
  into the document; on each `tick()` the node **reconciles** inputs under their
  owning project, so an input created on one node materializes on every peer.

- **Payload body on disk, not in the document.** The raw bytes are **not** in the
  CRDT — that would bloat every node's copy. Like a [large object's](large-objects.md)
  body, the payload lives on disk and is fingerprinted by `content_hash`. On a
  peer that has received the *record* but not (yet) the *body*,
  `ProjectInput::has_body()` is `false` and `read_payload()` returns
  `std::nullopt`. **Peer-to-peer body transfer is a later slice** (the metadata
  replicates today; the bytes do not move between nodes yet).

### On-disk layout

Each input is a folder named by its UUID under the project's `inputs/`
directory:

```
data_dir/
  <project-uuid>/
    project.json
    inputs/
      <input-uuid>/
        input.json   ← { id, project, kind, source, filename, content_hash,
                         size, created_by, created_at }
        payload      ← raw bytes (absent on a peer until body transfer exists)
```

`Node::start()` rebuilds the in-memory list by scanning each project's `inputs/`
folder (`Project::load()`) and — when replicated — reconciling from the CRDT, so
an input **survives restart** in both standalone and replicated nodes.

`input.json` is **validated at the boundary** on load: a malformed or incomplete
file (not an object, or an unknown `kind` / `source`) is skipped rather than
allowed to corrupt the in-memory tree — matching how `project.json` /
`database.json` / `table.json` are read.

### Integrity

The startup preflight (`Node::check_integrity()`) flags **dangling inputs** — an
`input:` key whose owning project is gone — alongside dangling tables/rows/large
objects, so orphaned input records don't accumulate in the document. (Payload
body presence is informational, like a large object's missing body — it may
simply be awaiting transfer.)

## Tests

`tests/test_project_input.cpp` covers create/fetch/drop with provenance
(computed `content_hash`, payload round-trip), survival across a restart (both
disk-only and CRDT-only), replication of an input record + a drop to a connected
peer (and that the body does **not** transfer yet), and boundary validation
(a malformed `input.json` is skipped on load).
