#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"
#include "zetta/storage/lazy_row.hpp"

// A `StorageEngine` over a Parquet body on disk (#833) — disk-backed, paging,
// and never holding the whole dataset in memory.
//
// `JsonlStore` reads every row into memory on construction, which is fine for a
// table and impossible for a partition body of a connector result set. A
// `ParquetStore` opens a `.parquet` file by its **footer only**, then decodes one
// **row group at a time** on demand, keeps the decoded pages in a cache bounded
// by a configurable byte budget, and hands rows back as
// [`ParquetLazyPtr`](../../../docs/data/lazy-rows.md) handles that stay valid
// after the cache reclaims the page they came from.
//
// **Read-only.** The body is an immutable artifact — a partition body written
// once by `write_partitioned`, or a Parquet file loaded as a dataset. `add`,
// `update` and `remove` are **refused**, not silently ignored: each records a
// path-qualified reason in `last_mutation_error()`, increments
// `rejected_mutations()`, and writes the reason to stderr. Mutating a dataset
// stored this way means writing a new body.
//
// Nothing here throws — every failure is a result with a reason. See
// docs/data/parquet-store.md.
namespace zetta::storage {

// The pager behind a ParquetStore: the file handle, the footer, the page cache.
// Defined in src/core/storage/parquet_store.cpp — a store shares it with every
// handle it hands out (as a `weak_ptr`), which is what lets a lazy handle
// re-read and what makes a handle outliving its store say so instead of
// dereferencing a dead one.
class ParquetPager;

// How a ParquetStore pages.
struct ParquetStoreOptions {
    // The decoded-page cache budget, in bytes of page footprint (an estimate:
    // the row group's uncompressed body plus a per-row overhead). Pages are
    // evicted least-recently-used until the resident total fits. A budget
    // smaller than one page still works — that page is decoded, used, and
    // reaped, so a scan costs one page of memory rather than the dataset.
    std::size_t page_cache_bytes = 64u * 1024u * 1024u;

    // The column carrying each row's stable id. When the body has no such
    // column, ids are synthesized as `<file stem>#<row index>` — stable for the
    // life of the body, since a body is immutable.
    std::string id_column = "_zid";

    // Whether the id column is part of the row data handed back by `at()`. A
    // partition body stores the id as a column and callers want the row without
    // it; a foreign Parquet file has no id column and this does not apply.
    bool hide_id_column = true;
};

// The outcome of opening a body: on success `store` is engaged and `error` is
// empty; on failure `store` is null and `error` says why (missing file,
// unreadable footer, an id column of the wrong type).
struct ParquetStoreResult {
    std::shared_ptr<class ParquetStore> store;
    std::string error;

    bool ok() const { return error.empty() && store != nullptr; }
};

class ParquetStore final : public StorageEngine {
public:
    // Open `path` as a storage engine. Reads the footer and the id column only —
    // row data is decoded lazily, a row group at a time.
    static ParquetStoreResult open(const std::filesystem::path& path,
                                   ParquetStoreOptions options = {});

    // Open a body already in memory (a partition body just read out of the
    // large-object store). The bytes are held for the store's lifetime, so this
    // trades the paging property for not needing a file — use `open` for
    // anything large.
    static ParquetStoreResult open_bytes(std::string bytes, std::string name,
                                         ParquetStoreOptions options = {});

    ~ParquetStore() override;

    const char* name() const override { return "parquet"; }

    // Refused — the body is immutable. Each records `last_mutation_error()`,
    // counts in `rejected_mutations()`, and reports on stderr.
    void add(std::string id, nlohmann::json doc) override;
    void update(const std::string& id, nlohmann::json doc) override;
    void remove(const std::string& id) override;

    bool contains(const std::string& id) const override;
    std::size_t size() const override;
    ParquetLazyPtr at(std::size_t i) const override;
    const std::string& id_at(std::size_t i) const override;

    // --- paging observability + control ------------------------------------

    // Total decoded-page footprint currently held **by the cache**. Pages a
    // caller still holds through a handle are not counted here — they belong to
    // the caller, and the cache has already let go of them.
    std::size_t resident_bytes() const;

    // Pages the cache is holding right now.
    std::size_t resident_pages() const;

    // How many times a page has actually been decoded off disk. A page
    // re-adopted through the cache's `weak_ptr` does not count — which is what
    // makes "no duplicate decode" an assertable property.
    std::uint64_t decode_count() const;

    // Number of pages (row groups) the body has.
    std::size_t page_count() const;

    // Drop every page the cache holds — exactly what an over-budget insert does,
    // exposed so a caller (and a test) can force it. Handles already given out
    // keep their pages alive; the cache keeps a `weak_ptr` so a later read
    // re-adopts rather than re-decodes.
    void reap();

    // The last refused mutation's reason, and how many have been refused.
    std::string last_mutation_error() const;
    std::uint64_t rejected_mutations() const;

private:
    explicit ParquetStore(std::shared_ptr<ParquetPager> pager);

    std::shared_ptr<ParquetPager> pager_;
};

}  // namespace zetta::storage
