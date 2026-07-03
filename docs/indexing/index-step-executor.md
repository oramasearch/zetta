# Running an index step — `IndexStepExecutor`

The [`IndexBackend` seam + `Index` entity](index-backend.md) (ENT-39 / ENT-40,
ADR-34) gave zetta the machinery to build, query, and record indexes, but left the
**run path** unwired: an `index` step in an [ExecutionGraph](../pipeline/execution-graph.md)
produced only a *logical* `index:<id>` placeholder. This slice (**#528**) closes
that gap — it drives a deployed `index` step through the resolved `IndexBackend`,
**actually building the index over the table's rows and serving queries from it**
(the slice `docs/indexing/index-backend.md` named as later work).

The acceptance: *an `index` step in a graph builds a real index that is then
queryable.*

## The per-node registry — `Node::index_registry()`

An [`IndexRegistry`](index-backend.md#the-registry--indexregistry) is the per-node
container + selector over the `IndexBackend` seam. Until now it was a standalone
seam; this slice gives the **`Node`** one (`Node::index_registry()`), constructed
with the always-available reference backends already registered:

| Backend | Name | Kind | When |
|---|---|---|---|
| [`InMemoryIndexBackend`](index-backend.md#the-reference-backend--inmemoryindexbackend) | `memory_search` | `search` | always (pure host C++) |
| [`OramaCoreFulltextIndex`](oramacore-fulltext-index.md) | `oramacore_fulltext` | `search` | when `ZETTA_WITH_ORAMACORE` (the private submodule was built) |

This is the same shape a [device registry](../compute/device-registry.md) takes — the CPU
device is always enumerated, the accelerator when built — so an `index` step
resolves to a backend out of the box. A host may `add()` more backends (vector /
semantic / feature) without touching this code.

`Node::index_registry()` is the live **machinery** that builds + serves indexes;
`Node::indexes()` is the durable **records** of which indexes exist (`index:<uuid>`).
They meet at `IndexSpec`: the registry resolves an `index`-step spec (or an
`Index` entity's `spec()`) to the backend that builds it.

## The executor — `IndexStepExecutor`

`IndexStepExecutor` (`include/zetta/index/index_step_executor.hpp`) is a
[`StepExecutor`](../pipeline/execution.md) — the pluggable unit a
[`GraphExecutor`](../pipeline/execution.md) runs each step with, the same seam
[`ChurnStepExecutor`](../use-cases/churn-step-executor.md) uses for real churn compute. The
orchestrator's walk is unchanged; only an `index` step gets real behavior:

```cpp
zetta::index::IndexStepExecutor step{node};                       // over node.index_registry()
zetta::pipeline::GraphExecutor executor{std::make_shared<zetta::index::IndexStepExecutor>(node)};
auto result = executor.execute(node, graph);               // builds the index step's index
```

For an `index` step it:

1. **extracts the `IndexSpec`** from the step's `attributes` bag
   ([`index_spec_from_node`](index-backend.md#resolving-the-ir-index-step) — the
   validated `{ table, kind, backend, fields }` boundary). A malformed / non-index
   node fails the step with a reason.
2. **resolves the backend** from the registry — the registered backend whose
   `name()` == `spec.backend` *and* whose `kind()` == `spec.kind`. An unknown
   backend (or a kind mismatch) fails the step.
3. **resolves the source table** named by `spec.table`. A table in zetta lives in
   a database, so it is resolved within the `database` the step optionally names
   (an extra `"database"` attribute), else by scanning the node's databases for
   the first table with that name. A missing table fails the step.
4. **reads the table's rows into `IndexPartition`s** (`read_index_partitions` —
   one `IndexDocument{ id, doc }` per row, chunked at `JsonlStore::kRecordsPerChunk`
   so the partitions line up with the table's on-disk chunks and a build can fan
   across cores/nodes in a later slice).
5. **builds the index** over those partitions through the backend
   (`backend->build(spec, partitions)`). A failing `IndexStatus` fails the step.
6. **records the durable `Index` entity** (`node.create_index` — validated at its
   [JSON-Schema boundary](index-backend.md#boundary-validation-json-schema)).
7. **emits an `index` Run output** referencing the **real** `Index` id (resolvable
   via `node.find_index(ref)`), not a logical placeholder.

Every other step kind delegates to [`DefaultStepExecutor`](../pipeline/execution.md), so the
executor is a drop-in that can run a whole `connect → … → index` graph while making
only the `index` step real.

### The built index is queryable

After the run, the built index lives in the registry backend keyed by
`IndexSpec::key()`, so it is **queryable for the node's lifetime**:

```cpp
auto index   = node.find_index(result.run->outputs()[0].ref);   // the recorded entity
auto backend = node.index_registry().resolve(index->spec());    // its backend

zetta::index::IndexQuery q;
q.field = "plan";
q.term  = "pro";
auto hits = backend->query(index->spec(), q);                   // the matching row ids
```

`tests/index/test_index_step_executor.cpp` is the acceptance: it builds an index over a
seeded `main.accounts` table through a graph's `index` step, then queries the built
index and asserts the returned row ids match the table's rows for that field/value
— *built and queryable*. It also covers the failure boundaries (unresolved backend,
missing source table, invalid spec) and the explicit-registry constructor.

## Churn: the `index` step builds a real index too

[`ChurnStepExecutor::run_index`](../use-cases/churn-step-executor.md) used to *only* score/rank
(the `index → score/rank` mapping the [Python frontend](../pipeline/python-frontend.md) and the
churn pipeline share, deliberately). It now **also builds a real, queryable entity
index** over the accounts shard through `node.index_registry()` — keyed by the
`entity` column — so a retention team can resolve an at-risk `entity` back to its
account record. The ranked at-risk list is still produced (the `act` step joins it
with each entity's dominant driver); the run's `index` output now references the
**real `Index` id** instead of a logical handle. When no Search backend is
registered the build is skipped and the at-risk list is still produced, so the
change is backward compatible.

## No new JSON format

The `index` step's attributes ride the already-IR-validated
[graph JSON](../pipeline/execution-graph.md) (read through `index_spec_from_node`), and the
recorded `Index` entity is validated at its own
[JSON-Schema boundary](index-backend.md#boundary-validation-json-schema)
(`parse_index_value`) — so malformed data still fails fast there. This slice adds no
parser of its own.

## What this slice is (and isn't)

It is **the run-path wiring**: a deployed `index` step builds a real index over a
table and serves queries. It is **not** the distributed build — `read_index_partitions`
chunks the rows so a future slice can fan the build across cores/nodes (today the
build is local), and the vector / semantic / feature backends remain additive
behind the same `IndexBackend` interface.

## Pointers

- [`include/zetta/index/index_step_executor.hpp`](../../include/zetta/index/index_step_executor.hpp)
  / [`src/core/index/index_step_executor.cpp`](../../src/core/index/index_step_executor.cpp)
  — the executor + `read_index_partitions`.
- [index-backend.md](index-backend.md) — the seam, registry, reference backend, and
  the `Index` entity this slice drives.
- [execution.md](../pipeline/execution.md) — the `GraphExecutor` / `StepExecutor` seam.
- [churn-step-executor.md](../use-cases/churn-step-executor.md) — the churn `index` step that now
  builds a real index alongside the at-risk ranking.
- [oramacore-fulltext-index.md](oramacore-fulltext-index.md) — the full-text backend
  registered on the node when its submodule is built.
