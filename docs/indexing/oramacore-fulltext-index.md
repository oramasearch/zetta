# OramaCore full-text index backend (`zetta::index::OramaCoreFulltextIndex`)

The **first *real* [`IndexBackend`](index-backend.md)** — a **full-text (BM25F)**
Search backend backed by the native **[oramacore-cpp](#the-engine--oramacore-cpp)**
engine (ENT-39 / ENT-40, ADR-34, [#527](https://github.com/oramasearch/zetta/issues/527)).
Where [`InMemoryIndexBackend`](index-backend.md#the-reference-backend--inmemoryindexbackend)
is exact-value lookup (the seam's from-scratch reference Search backend), this one
is **ranked free-text relevance**: tokenize + stem + stop-word filter on both the
documents and the query, then per-field-weighted, length-normalized **BM25F**
scoring, so a query returns the row ids of the best-matching rows **ranked by
relevance**.

It plugs into the same [`IndexRegistry`](index-backend.md#the-registry--indexregistry)
as any other backend — selector name **`oramacore_fulltext`**, kind **`Search`** —
and stays behind the `IndexBackend` seam: like every seam call it **returns an
`IndexStatus`, never throws** (zetta-core is `-fexperimental-library` and must not
unwind across a seam).

## The engine — oramacore-cpp

[`oramacore-cpp`](https://github.com/oramasearch/oramacore-cpp) is the **C++23 port
of OramaCore's full-text engine** (BM25F first), vendored as a git submodule under
`modules/oramacore-cpp` (see [`modules/README.md`](../../modules/README.md)). The
backend uses its `oramacore::index::MemoryIndex`: an in-memory BM25F index you
`add(Document)` text rows to and `search(query, top_k)` for ranked `Hit{doc_id,
score}`. **BM25F** differs from plain BM25 in two ways — per-field **weight** (boost
a field) and per-field **length-normalization** (normalize by *that* field's average
length) — summed into a pseudo-tf before the saturation term. Tokenization is a
self-contained normalize → split → stop-word → minimal-stemmer pipeline (no foreign
NLP runtime), applied identically to documents and queries.

## How the seam maps onto BM25F

| `IndexBackend` concept | oramacore-cpp |
|---|---|
| `IndexSpec.fields` | one BM25F field per indexed field (default weight 1.0, b 0.75) |
| `IndexPartition` → `IndexDocument{id, doc}` | each row's `spec.fields` values → a `MemoryIndex::Document` (field → text), `add`ed in partition/row order |
| row id (`_zid`, a string) ↔ engine `DocId` (a `uint32`) | a `rowid_by_docid` vector — the engine speaks dense ids, the seam speaks row ids |
| `IndexQuery.term` (a JSON value) | the free-text query string; numbers/booleans stringify to their token form |
| `IndexQuery.top_k` | the engine's `top_k` cap (0 = no cap) |
| `IndexQueryResult.row_ids` | the hits' row ids, **ranked** (score desc, stable tiebreak) |

### build

`build(spec, partitions)`:

- **Rejects** a non-`Search` spec (`Unsupported`) and an empty `fields`
  (`InvalidArgument`) — same contract as the in-memory backend.
- Creates a `MemoryIndex` with one BM25F field per `spec.fields` entry (default
  params), then walks every partition's documents in order: each row's indexed
  field values are rendered to text (strings pass through; numbers/booleans
  stringify; null/array/object contribute nothing — a tolerant build over free-form
  rows) and `add`ed. A row missing the indexed fields is tolerated (it simply never
  matches). Replaces any index previously built under `spec.key()`.

### query

`query(spec, query)`:

- `query.term` is the **free-text query**, scored by BM25F across **all** of the
  index's fields (BM25F weights them per-field). The row ids come back **ranked by
  relevance** (score desc, then the engine's stable DocId tiebreak — deterministic
  and reproducible because the build order is deterministic), capped to
  `query.top_k`.
- When set, `query.field` must be one of the index's fields (else
  `InvalidArgument`), and **only that field is searched**
  ([lake#782](https://github.com/oramasearch/lake/issues/782)): a row whose match
  is in another field is not a hit, and the score is BM25 over that field alone.
  The engine's `search` always scores across every field, so a build over more
  than one field also keeps one single-field `MemoryIndex` per field, fed every
  row in the same order (a row without the field as an empty document) so its
  DocIds map through the same row-id vector. That costs a second copy of the
  postings; an index over one field keeps none, since the combined index is it.
- A term that is not a text/scalar value (an array/object/null) → `InvalidArgument`;
  a stop-word-only or no-match query → an **empty but successful** result;
  querying before a build → `NotFound`.

### drop

`drop(spec)` removes the index built under `spec.key()`; **idempotent** (Ok even if
none exists).

## CPU-only / private-submodule safe (the build guard)

`oramacore-cpp` is a **private** submodule, so a checkout that did not (or could
not) initialize it — a CPU-only CI that lacks repo access — must still configure,
build, and run **green**. CMake guards the engine on the submodule's presence:

```cmake
set(ZETTA_ORAMACORE_DIR "${CMAKE_SOURCE_DIR}/modules/oramacore-cpp")
if(EXISTS "${ZETTA_ORAMACORE_DIR}/CMakeLists.txt")
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)   # don't build its Catch2 tests
  add_subdirectory("${ZETTA_ORAMACORE_DIR}" EXCLUDE_FROM_ALL)
  # ... -fexperimental-library to match zetta-core's libc++ ABI ...
  set(ZETTA_WITH_ORAMACORE ON)
endif()
# zetta-core links orama::oramacore-cpp PRIVATE + defines ZETTA_WITH_ORAMACORE=1
# only when present.
```

- **When the submodule is present** (`ZETTA_WITH_ORAMACORE=1`): the engine is built
  (CPU-only, no GPU/CUDA) and linked **PRIVATE** to `zetta-core` — only
  `src/core/index/oramacore_fulltext_index.cpp` touches it, so its headers stay out
  of zetta-core's public surface, exactly like the JSON-Schema validator. The
  backend's built indexes live behind a **pImpl** so no oramacore-cpp type appears
  in the public header.
- **When the submodule is absent**: the backend stays a usable, registerable
  `IndexBackend`, but `build()`/`query()` return `Unsupported` (with a clear "rebuild
  with the submodule present" reason) and `drop()` is `Ok`. The header is includable
  and the build links + runs unchanged.

`OramaCoreFulltextIndex::engine_available()` reports which path a build took (true
iff `ZETTA_WITH_ORAMACORE` was defined). Initialize the submodule with:

```bash
git submodule update --init modules/oramacore-cpp
```

Because the engine is compiled with `-fexperimental-library` (matching the
`Boost::capy`-propagated libc++ ABI of zetta-core), std containers cross the
`zetta-core → oramacore-cpp` link boundary safely — the same guard the JSON-Schema
validator gets.

## No new JSON boundary

This slice introduces **no new JSON format**. The backend operates over inputs that
are already validated elsewhere: the [`IndexSpec`](index-backend.md) (from the
[`Index` entity](index-backend.md#the-entity--index)'s schema, or
`index_spec_from_node` at the IR boundary) and the row documents (the storage
engine's already-stored rows). The `IndexQuery.term` is a runtime query value, not a
stored/replicated payload. So there is no schema to add here — validation lives at
the boundaries that produce a spec.

## What this slice is (and isn't)

It is the **first real index backend**: a full-text BM25F Search backend, usable
end-to-end through the registry (resolve an IR `index` step naming `oramacore_fulltext`
→ build over partitions → query for a ranked list), plus the CMake build/link of the
`oramacore-cpp` submodule, guarded so CPU-only CI stays green. Driving a *deployed*
`index` step through the resolved backend (one consolidated index over the table's
partitions) is the [`IndexStepExecutor`](index-step-executor.md) slice (#528); the
**per-partition distributed** form — a sub-index per LOCAL partition answered by
scatter-gather + merge/rank, the default backend of
[`DistributedFulltextIndex`](distributed-fulltext.md) — is #529. The seam's `query`
now returns per-hit BM25F **scores** (aligned to `row_ids`) so the distributed merge
can rank globally. Per-field BM25F weights/params are defaults today (the `IndexSpec`
carries only `fields`); exposing them is a future, additive change. Other index
types (vector / semantic / feature) remain additive behind the same `IndexBackend`
interface.

## Pointers

- [`include/zetta/index/oramacore_fulltext_index.hpp`](../../include/zetta/index/oramacore_fulltext_index.hpp) /
  [`src/core/index/oramacore_fulltext_index.cpp`](../../src/core/index/oramacore_fulltext_index.cpp) —
  the backend (pImpl over the engine; `ZETTA_WITH_ORAMACORE`-guarded).
- [`tests/index/test_oramacore_fulltext_index.cpp`](../../tests/index/test_oramacore_fulltext_index.cpp) —
  build + query a full-text index over a small table (the acceptance), the
  registry/IR-step end-to-end path, and the engine-absent degradation.
- [index-backend.md](index-backend.md) — the `IndexBackend` seam + `IndexRegistry` +
  the `Index` entity this backend plugs into.
- [`modules/README.md`](../../modules/README.md) — the vendored `oramacore-cpp` submodule.
- [execution-graph.md](../pipeline/execution-graph.md) — the IR whose `index` step resolves to a backend.
