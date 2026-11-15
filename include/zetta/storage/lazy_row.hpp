// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// The row handle every storage engine hands back from `at()` (#833).
//
// An in-memory engine can return `const nlohmann::json&` because it owns the row
// for its whole lifetime. A **disk-backed, paging** engine cannot: it decodes a
// page to answer the read, and if it later reclaims that page every reference it
// handed out dangles. `ParquetLazyPtr` is the handle that closes that hole:
//
//     ParquetLazyPtr {
//       key  : (which body, which page, which row)  // always present
//       page : shared_ptr<const RowPage>            // engaged when resident
//     }
//
// **The refcount decides; nothing arbitrates.** "Reaping" is the store dropping
// *its own* `shared_ptr` to a page. A page a caller is still reading survives
// until that caller's handle goes away, and then frees itself. A handle can
// therefore never observe freed memory — by construction, not by documentation.
// The store's cache keeps a `weak_ptr` per page after reaping, so the next
// request for that page `lock()`s the copy that is still alive instead of
// decoding a second one.
//
// A handle can also give up residency itself (`release()`), keeping only the
// key, and materialize again later — that re-read touches the disk and **can
// fail**, so `materialize()` reports a status and `error()` carries a
// path-qualified reason. It deliberately does *not* return null for a failed
// read: every other null-returning lookup in zetta (`find_table`, `find_model`)
// means *absence*, and a corrupt page is data loss, not absence.
//
// Nothing here throws — zetta-core is compiled `-fexperimental-library`.
//
// See docs/data/lazy-rows.md.
namespace zetta::storage {

// One decoded page of rows — the unit a paging engine reads, caches, and
// reclaims. `ids` and `rows` are parallel: `ids[i]` is the stable row id of
// `rows[i]`. `resident_bytes` is the page's approximate memory footprint, which
// is what a cache budget is spent against.
struct RowPage {
    std::vector<std::string> ids;
    std::vector<nlohmann::json> rows;
    std::size_t resident_bytes = 0;
};

// Pages are shared and immutable once decoded: a handle and the store's cache
// hold the same bytes, and whoever releases last frees them.
using RowPagePtr = std::shared_ptr<const RowPage>;

// Where a row lives, independent of whether it is currently in memory. A key is
// always present in a handle, which is what makes a lazy handle able to
// re-materialize: `blob` selects the body inside the engine, `page` the page
// within that body (a Parquet row group), `index` the row within the page.
struct RowKey {
    std::uint32_t blob = 0;
    std::uint32_t page = 0;
    std::uint64_t index = 0;
};

// The outcome of decoding (or re-adopting) a page: on success `error` is empty
// and `page` is engaged; on failure `page` is null and `error` says why.
struct RowPageResult {
    RowPagePtr page;
    std::string error;

    bool ok() const { return error.empty() && page != nullptr; }
};

// What a lazy handle re-reads through. A paging engine owns one of these as a
// `shared_ptr` and hands handles a `weak_ptr` to it, so a handle that outlives
// its engine reports "the store is gone" instead of dereferencing a dead one.
class RowPageSource {
public:
    virtual ~RowPageSource() = default;

    // Decode the page named by `key` — or re-adopt the live copy of it, if one
    // is still alive. Non-throwing; `key.index` is not consulted.
    virtual RowPageResult fetch_page(const RowKey& key) const = 0;
};

// A reference to one stored row: its data and its id.
//
// Three states, all of them safe to hold:
//
//   * **borrowed** — the row is owned by an engine that keeps it resident for
//     its own lifetime (`JsonlStore`, `JsonStore`). Two raw pointers, no
//     allocation, no refcount: the always-resident common path costs nothing.
//   * **paged** — the row lives in a `RowPage` this handle keeps alive. The
//     store may reap the page at any moment; the row stays readable here.
//   * **lazy / failed** — no row in hand. `error()` is empty for a handle that
//     released its page on purpose and non-empty for one whose read failed. A
//     handle returned by `StorageEngine::at()` is never lazy: it is either
//     dereferenceable or carries a reason.
//
// `ok()` (and `operator bool`) means *dereferenceable right now*. `operator*`,
// `operator->` and `id()` have `ok()` as their precondition.
class ParquetLazyPtr {
public:
    // An empty handle: no row, no error. Dereferencing one is a precondition
    // violation, like `at(size())`.
    ParquetLazyPtr() = default;

    // Borrowed: the engine guarantees `row` and `id` outlive every handle it
    // hands out (they are its own storage). Allocation-free.
    ParquetLazyPtr(const nlohmann::json& row, const std::string& id) noexcept
        : row_(&row), id_(&id) {}

    // Paged and resident: the handle holds `page` alive for as long as it lives.
    // `source` lets it come back after `release()`.
    ParquetLazyPtr(RowKey key, RowPagePtr page,
                   std::weak_ptr<const RowPageSource> source);

    // Lazy: only the key. The first `materialize()` reads the page in.
    ParquetLazyPtr(RowKey key, std::weak_ptr<const RowPageSource> source);

    // A handle carrying why the row could not be read (a corrupt page, an index
    // past the end, a body that vanished). Never dereferenceable.
    static ParquetLazyPtr failed(std::string reason);

    // Dereferenceable right now — no I/O needed, no error pending.
    bool ok() const noexcept { return row_ != nullptr; }
    explicit operator bool() const noexcept { return ok(); }

    // Why the last read failed; empty when nothing failed.
    const std::string& error() const noexcept { return error_; }

    // True for a handle that has a key and a source but no page in hand — it can
    // be brought back with `materialize()`.
    bool lazy() const noexcept { return row_ == nullptr && !source_.expired(); }

    // The row data. Precondition: ok().
    const nlohmann::json& operator*() const noexcept { return *row_; }
    const nlohmann::json* operator->() const noexcept { return row_; }

    // The row's stable id. Precondition: ok().
    const std::string& id() const noexcept { return *id_; }

    // The row data, or `fallback` when this handle holds no row — for a caller
    // that treats an unreadable row as an empty one (a schema sniff, a UI list).
    const nlohmann::json& value_or(const nlohmann::json& fallback) const noexcept {
        return row_ != nullptr ? *row_ : fallback;
    }

    // Where this row lives. Meaningful for a paged handle; zero for a borrowed
    // one (a borrowed row is not addressed by page).
    const RowKey& key() const noexcept { return key_; }

    // Bring the row back into memory if it is lazy; a no-op for a handle that
    // already holds one. Returns true when the handle is dereferenceable
    // afterwards, false with `error()` set when the re-read failed.
    bool materialize() const;

    // Give up residency, keeping the key and the source. The page is freed only
    // if nobody else (including the store's cache) still holds it. A borrowed
    // handle cannot be released — it has nothing to re-read through — so this is
    // a no-op for one.
    void release() noexcept;

private:
    // Mutable because materialize() is logically const: it changes where the row
    // is, never what it is.
    mutable const nlohmann::json* row_ = nullptr;
    mutable const std::string* id_ = nullptr;
    mutable RowPagePtr page_;
    mutable std::string error_;
    std::weak_ptr<const RowPageSource> source_;
    RowKey key_;
};

}  // namespace zetta::storage
