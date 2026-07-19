# Lazy rows — the handle `StorageEngine::at()` returns

`StorageEngine::at(i)` used to return `const nlohmann::json&`. An engine that
keeps every row in memory can do that: it owns the row for its whole lifetime, so
the reference is good for as long as the store is. A **disk-backed, paging**
engine cannot. It decodes a page to answer the read, and the moment it reclaims
that page every reference it handed out dangles.

`zetta::storage::ParquetLazyPtr` (`include/zetta/storage/lazy_row.hpp`, #833) is
the handle that closes the hole:

```
ParquetLazyPtr {
  key  : (which body, which page, which row)   // always present
  page : shared_ptr<const RowPage>             // engaged when resident, empty when lazy
}
```

The key is always there, so the handle can always re-materialize. The page is
there when the row is in memory.

## The rule: the refcount decides, nothing arbitrates

**"Reaping" is the store dropping its own `shared_ptr`** — that is the whole of
it. There is no callback, no pin count, no "is anyone using this?" question asked
of anybody. A page a caller is still reading is still referenced, so it survives;
when that last holder goes away the page frees itself. A handle can therefore
never observe freed memory — *by construction*, not because a rule says so:

```cpp
const auto row = store.at(i);   // decodes a page, holds it
store.reap();                   // the cache lets go of every page
use(*row);                      // still valid: `row` is the last holder
```

The store's cache keeps a **`weak_ptr`** next to each slot after reaping, so the
next request for that page `lock()`s the copy a handle is still holding rather
than decoding a second one. Two rows of the same page cost one decode even when
the cache has already given it up
(`LazyRow.ASecondRowOfALivePageReAdoptsItRatherThanDecodingTwice`).

## The three states

| State | What it holds | Cost |
|---|---|---|
| **borrowed** | raw pointers to a row + id an always-resident engine owns (`JsonlStore`, `JsonStore`) | no allocation, no refcount |
| **paged** | a `shared_ptr` to the `RowPage` the row lives in | one refcount bump per handle |
| **lazy / failed** | the key only | nothing |

The **borrowed** state is why the common path pays nothing for this. `JsonlStore`
already holds the row; `at(i)` returns `ParquetLazyPtr(records_[i], ids_[i])` —
two pointers, no page, no atomics.

## At a call site

A scan is still a scan:

```cpp
for (std::size_t i = 0; i < store.size(); ++i) {
    const auto row = store.at(i);
    if (!row) continue;              // row.error() says why
    use(*row, row.id());
}
```

`ok()` (and `operator bool`) means **dereferenceable right now**; `operator*`,
`operator->` and `id()` have it as their precondition. The handle carries the id
as well as the data, so a caller that wants both does one lookup.

## A failed read is a status, not a null

A lazy dereference re-reads from disk, and that can fail — a truncated body, a
corrupt page, a file that moved. `materialize()` returns **false with a
path-qualified `error()`**, and `at()` on a paging engine returns a handle in the
same failed state.

It deliberately does not return null. Every other null-returning lookup in zetta
(`find_table`, `find_model`) means *absence*, and callers are trained to read
null that way. A corrupt page is data loss; making it indistinguishable from
"not there" would turn a loud failure into a quiet wrong answer. Nothing throws:
zetta-core is compiled `-fexperimental-library`.

Callers with an error channel propagate the reason (`SELECT` fails the query, the
DDF loader returns a `ComputeStatus`, `write_partitioned` fails the write).
Callers without one — the row-detail HTTP route, the GUI explorer, the index
partition reader — say so in their own terms and are marked with a comment where
they do.

## Giving up residency on purpose

A holder that wants to keep a reference for a long time without pinning a page
calls `release()`: the page reference is dropped (freeing it if nobody else holds
it), the key stays, and `materialize()` brings it back later. A handle whose
store has since been destroyed reports *"the store that owns page N is gone"*
rather than dereferencing a dead one — handles hold a `weak_ptr` to the page
source, never a raw one.

A **borrowed** handle has no source to re-read through, so `release()` is a no-op
for it: the row it points at is owned by a store that keeps it resident anyway.

## Implementing a paging engine

Implement `RowPageSource::fetch_page(key)` — decode (or re-adopt) the page named
by the key — and hand out `ParquetLazyPtr(key, page, weak_from_this())`. The one
in tree is [`ParquetStore`](parquet-store.md). The contract a source owes:

- a page is **immutable** once decoded (it is shared, and shared mutable state is
  the bug this design exists to avoid);
- `fetch_page` is **non-throwing** and reports a reason on failure;
- cache slots that are read and reassigned across threads are
  `std::atomic<std::shared_ptr<Page>>`, so a reader either sees the old page or
  the new one, never a torn pointer.

## Tests

`tests/data/test_lazy_row.cpp` drives the states against a counting stub source
(a page decoder that counts decodes and can be told to fail), independent of
Parquet:

- borrowed rows out of `JsonlStore` / `JsonStore`, and a past-the-end index
  returning a **failed handle** rather than throwing;
- **a handle read after a reap** — the property the whole design exists for;
- the page freeing itself when the last handle goes, and *not* before;
- a second row of a reaped-but-live page re-adopting it (one decode, not two);
- `release()` → `materialize()` re-reading (the decode counter proves it);
- a failed re-read reporting a reason, and recovering when the read does;
- a lazy handle outliving its store saying so, and a **resident** handle
  outliving its store still being readable.
