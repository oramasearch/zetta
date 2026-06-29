# Indexes — the IndexBackend seam + the Index entity

An **index** in zetta is *what accelerates a lookup over a table* — a search
index, a vector (semantic) index, a feature index. This slice (**ENT-39 / ENT-40,
ADR-34**) delivers two halves:

1. The **`IndexBackend` seam** — the pluggable interface every concrete index
   implementation targets (**build** over a table's partitions, **query**,
   **drop**), plus the **`IndexRegistry`** that resolves an index to its backend
   and a from-scratch **in-memory reference backend**. New index types are
   *additive*: a vector / semantic backend drops in behind the same interface
   without touching the registry or the IR.
2. The **`Index` entity** — a top-level, **replicated** record
   `{ table, kind, backend, fields }` keyed by id (`index:<uuid>`), with `Node`
   CRUD, an on-disk projection, JSON-Schema boundary validation, and a
   `system.zetta_indexes` registry table — modeled exactly like
   [`zetta::Model`](models.md) (minus the weights body: an index has no
   large-object body, only metadata).

The seam is *how an index is built and queried*; the entity is *the durable
record of which indexes exist*. They meet at `IndexSpec`: the entity hands a
backend its `spec()`, and the registry resolves a `spec` (or an IR `index` step)
to the backend that should build it.

## The kind taxonomy

`IndexKind` (`include/zetta/index/index_kind.hpp`) is the closed set of index
*types*, mirroring the `StepKind::Index` doc comment ("build a search / vector /
feature index over the results"):

| Kind | Name | What it accelerates |
|---|---|---|
| `Search` | `search` | inverted / search index over field values (exact + text lookup) |
| `Vector` | `vector` | nearest-neighbour index over embedding vectors (semantic search) |
| `Feature` | `feature` | feature index / store keyed by entity for ML serving |

The lowercase name is both the display label and the stored JSON token. It is a
header-only enum + `to_string` / `index_kind_from_name`, the same foundational
shape as `ir::StepKind` and `compute::DataType`.

## The seam — `IndexBackend`

`IndexBackend` (`include/zetta/index/index_backend.hpp`) is a pure-virtual base
class whose concrete subclasses are the backends — the same shape as zetta's
other seams (`StorageEngine`, `compute::ComputeDevice`, `StepExecutor`, `Check`).
Like `ComputeDevice`, every call **returns a status rather than throwing**
(`IndexStatus`, codes `Ok` / `Unsupported` / `InvalidArgument` / `NotFound` /
`BackendError`) — zetta-core compiles with `-fexperimental-library` and must never
unwind across a seam.

```cpp
class IndexBackend {
public:
    virtual std::string name() const = 0;   // selector name (matches Index::backend)
    virtual IndexKind kind() const = 0;      // the kind it implements

    // build over a table's partitions, query a built index, drop it.
    virtual IndexStatus build(const IndexSpec& spec,
                              std::span<const IndexPartition> partitions) = 0;
    virtual IndexQueryResult query(const IndexSpec& spec,
                                   const IndexQuery& query) const = 0;
    virtual IndexStatus drop(const IndexSpec& spec) = 0;
};
```

- **`IndexSpec`** `{ table, kind, backend, fields }` is the seam's view of an
  index (the entity carries the same four fields). Its `key()` — `table + kind +
  fields`, **not** `backend` — identifies an index within a backend that may hold
  many (`backend` selects the *implementation*, not the index).
- **build over partitions.** An `IndexPartition` is a chunk of the table's rows
  (id + `IndexDocument{ id, doc }` each) — the unit a build ingests, so a build
  can fan across cores/nodes (a later slice). A table is stored in chunks
  ([JsonlStore](storage.md), 10k/chunk) and read into [DDF](ddf.md) chunks; those
  are its partitions.
- **query** takes an `IndexQuery` (a `field` + `term` for Search; a `vector` +
  `top_k` for Vector/semantic — declared now so the seam is stable when those
  backends land, additive: a field a backend ignores is simply unused) and returns
  the matching row ids in deterministic order.

### The reference backend — `InMemoryIndexBackend`

`InMemoryIndexBackend` (`include/zetta/index/in_memory_index_backend.hpp`) is a
from-scratch, in-memory **Search** backend (name `memory_search`) that makes the
seam end-to-end usable and gives future backends a concrete template — the role
`compute::HostStubDevice` / `CpuDevice` play for the `ComputeDevice` seam. It
builds an **inverted index**: for each indexed field, `build` maps value → the row
ids carrying it, across all partitions (deduped + sorted, so queries are
reproducible); `query` returns the row ids whose `field` equals `term` (capped to
`top_k`); `drop` is idempotent. It lives in zetta-core (pure host C++). Real
search (tokenization, ranking, on-disk segments) and the vector / feature backends
are later slices behind the same seam.

## The registry — `IndexRegistry`

`IndexRegistry` (`include/zetta/index/index_registry.hpp`) is the per-node
container + selector over the seam — the same shape as
[`compute::DeviceRegistry`](device-registry.md). Backends are *registered* into
it; the registry does not manufacture them. Its job is **resolution**:

```cpp
IndexRegistry registry;
registry.add(std::make_shared<InMemoryIndexBackend>());

// Resolve by spec: the backend whose name() == spec.backend AND kind() == spec.kind.
auto backend = registry.resolve(some_index.spec());

// Resolve straight from an IR `index` step — the acceptance path.
auto from_step = registry.resolve(index_step_node);
```

`resolve` returns the registered backend whose **name matches** the spec's
`backend` **and whose kind matches** the spec's `kind` — a name match with a kind
mismatch is *not* a resolution (the spec asked for a kind that backend does not
build), and an unknown backend resolves to `nullptr` (a safe handle, never a raw
pointer — the house style).

### Resolving the IR `index` step

`index_spec_from_node(const ir::Node&)` extracts an `IndexSpec` from an
[ExecutionGraph IR](execution-graph.md) `index` step's free-form `attributes` bag:

```json
{ "table": "accounts", "kind": "search",
  "backend": "memory_search", "fields": ["plan", "region"] }
```

It validates at the boundary — `nullopt` unless the node is a `StepKind::Index`
node with a non-empty `table`, a known `kind`, a non-empty `backend`, and a
non-empty `fields` array of unique non-empty strings — so a malformed or non-index
node never silently resolves. (It reads an already-IR-validated attribute bag, so
it is a tolerant hand check, not a second JSON Schema.) `IndexRegistry::resolve(const
ir::Node&)` composes the two: **the IR `index` step resolves to a backend**, the
acceptance criterion.

## The entity — `Index`

`Index` (`include/zetta/index/index.hpp`) is a top-level entity with no parent,
modeled like [`zetta::Model`](models.md) — keyed by id (indexes have no human-name
lookup; the registry is `Node::indexes()` + the system table). Its value
(`IndexValue`):

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `table` | `std::string` | **Required, non-empty** — the source table the index is over. |
| `kind` | `IndexKind` | **Required** — `search` / `vector` / `feature`. |
| `backend` | `std::string` | **Required, non-empty** — the `IndexBackend` name that builds it. |
| `fields` | `vector<string>` | **Required, ≥ 1, unique** — the fields it indexes. |
| `created` | `std::string` | ISO-8601 UTC stamp at creation. |

`IndexValue::spec()` returns the `IndexSpec{ table, kind, backend, fields }` view —
the bridge from the durable entity to the seam (`registry.resolve(index->spec())`).

### Lifecycle (on a Node)

`Node` owns indexes like it owns [models](models.md):

```cpp
zetta::IndexValue spec;
spec.table = "accounts";
spec.kind = zetta::IndexKind::Search;
spec.backend = "memory_search";
spec.fields = {"plan", "region"};
auto index = node.create_index(spec);     // empty handle if table/backend/fields are empty

auto found = node.find_index(index->id()); // by id; empty handle on miss
auto all   = node.indexes();               // the registry (created, then id)
bool gone  = node.drop_index(index->id()); // remove from CRDT + disk
```

`create_index` stamps `created`, captures `index:<id>` into the CRDT (when
replicated), and writes the `index.json` projection. An index's definition is
immutable (changing what it indexes is a *new* index — an index is rebuilt, not
re-specified), so there are no value mutators; reconciliation uses `apply_value()`,
which rewrites the projection from the document with **no capture**.

### On disk

Each index is a folder named by its UUID directly under `data_dir`, holding only
the metadata projection (no body):

```
data_dir/
  <index-uuid>/
    index.json   { id, table, kind, backend, fields, created }
```

`index.json` is a projection of the replicated `index:<uuid>` value (plus the
`id`); standalone nodes read/write it directly.

### Replication

Following zetta's **"everything is replicated state"** rule, the index metadata
goes through `ReplicationManager` like every other entity: `index:<uuid>` → the
value above rides in the Automerge CRDT and gossips between peers; `Node` captures
local creates and reconciles peer changes into the on-disk projection on each
`tick()`. An index is **metadata only** — there is no body to transfer (unlike a
[model](models.md)'s weights), so a single-node and a multi-node cluster converge
on the same index records. (The built index data structures themselves are the
backend's concern, rebuilt per node — building a deployed index is a later slice.)

### Boundary validation (JSON Schema)

`Index` is validated against a formal **JSON Schema** (draft-07) at the storage
boundary — the same discipline as the [Run](runs.md) / [Model](models.md) records:
`table` / `backend` are required + non-empty, `kind` is a closed enum, `fields` is
a non-empty array of **unique** non-empty strings, and no stray fields are allowed
(`additionalProperties: false`). Every value entering from disk (`index.json`) or
the CRDT (`index:<id>`) passes `parse_index_value`, so a malformed record is
rejected with a clear reason (logged + skipped on load) instead of corrupting the
in-memory tree. The embedded schema is the **source of truth**, exposed via
`index_value_schema()`; `schemas/index.schema.json` is generated from it and
drift-checked (`tests/test_schemas.cpp`) — see [schemas.md](schemas.md). Validation
runs via `pboettch/json-schema-validator` (the same path as Run/Model), linked
PRIVATE to `zetta-core`.

### The registry — `system.zetta_indexes`

The index registry is surfaced as a read-only [system table](query.md) (like
`system.zetta_models`), computed at query time from `Node::indexes()`:

```sql
SELECT * FROM system.zetta_indexes;
SELECT index_id, kind FROM system.zetta_indexes WHERE kind = 'search';
```

Columns: `index_id`, `table`, `kind`, `backend`, `fields` (a JSON array, like
`system.zetta_models`' `feature_schema`), `field_count`, `created`.

## What this slice is (and isn't)

It is **the seam + the registry + a reference backend + a replicated, validated,
queryable `Index` entity**. It is **not** yet wired into the run/deploy path: the
`IndexRegistry` is a standalone seam (like the [Provisioner](provisioner.md) /
[DeviceRegistry](device-registry.md)), and driving a deployed `index` step through
the resolved backend — actually building the index over the table's partitions and
serving queries from it — is a later slice. The vector / semantic / feature
backends (additive behind the same `IndexBackend` interface) are also later slices.

## Pointers

- [`include/zetta/index/index_kind.hpp`](../include/zetta/index/index_kind.hpp) —
  the `IndexKind` taxonomy.
- [`include/zetta/index/index_backend.hpp`](../include/zetta/index/index_backend.hpp) —
  the `IndexBackend` seam + `IndexSpec` / `IndexStatus` / partition / query types.
- [`include/zetta/index/index_registry.hpp`](../include/zetta/index/index_registry.hpp) —
  the `IndexRegistry` + `index_spec_from_node`.
- [`include/zetta/index/in_memory_index_backend.hpp`](../include/zetta/index/in_memory_index_backend.hpp) —
  the in-memory reference Search backend.
- [`include/zetta/index/index.hpp`](../include/zetta/index/index.hpp) /
  [`src/core/index/index.cpp`](../src/core/index/index.cpp) — the `Index` entity +
  JSON-Schema boundary.
- [models.md](models.md) — the sibling top-level entity this is modeled on.
- [device-registry.md](device-registry.md) — the registry whose shape `IndexRegistry` mirrors.
- [execution-graph.md](execution-graph.md) — the IR whose `index` step resolves to a backend.
- [query.md](query.md) — the system-table mechanism behind `system.zetta_indexes`.
