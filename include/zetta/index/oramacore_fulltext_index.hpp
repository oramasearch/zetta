#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "zetta/index/index_backend.hpp"

// OramaCoreFulltextIndex (ENT-39 / ENT-40, ADR-34, #527) — the first *real*
// IndexBackend: a **full-text (BM25F)** Search backend backed by the native
// oramacore-cpp engine (modules/oramacore-cpp), the C++23 port of OramaCore's
// full-text core. Where InMemoryIndexBackend is exact-value lookup (the seam's
// reference Search backend), this one is ranked free-text relevance — tokenize +
// stem + stop-word filter on both sides, then per-field-weighted, length-
// normalized BM25F scoring — so a query returns the row ids of the best-matching
// rows, ranked.
//
// It plugs into the same IndexRegistry as any other backend (name
// "oramacore_fulltext", kind Search) and stays behind the IndexBackend seam: like
// every seam call it **returns a status, never throws** (zetta-core is
// -fexperimental-library and must not unwind across a seam). oramacore-cpp types
// never appear in this header — the built indexes live behind a pImpl so the
// heavy dependency stays confined to the .cpp (the same discipline that keeps
// SDL/ImGui/CUDA out of zetta-core's surface), and the header is includable in any
// build, with or without the engine.
//
// **CPU-only / private-submodule safe.** oramacore-cpp is a *private* submodule, so
// a checkout that did not (or could not) initialize it must still configure, build,
// and run green. CMake guards the engine behind `ZETTA_WITH_ORAMACORE`: when the
// submodule is present the real engine is linked; when it is absent build()/query()
// degrade to `Unsupported` (drop() is still Ok — nothing was built). `engine_available()`
// reports which. See docs/indexing/oramacore-fulltext-index.md.
namespace zetta {

class OramaCoreFulltextIndex final : public IndexBackend {
public:
    // The backend's stable selector name — what an Index::backend / IndexSpec
    // names to resolve to this backend in an IndexRegistry.
    static constexpr std::string_view kName = "oramacore_fulltext";

    OramaCoreFulltextIndex();
    ~OramaCoreFulltextIndex() override;
    // Move-only (it owns built-index state through a pImpl). Not copyable — a
    // built BM25F index is not a value to clone.
    OramaCoreFulltextIndex(OramaCoreFulltextIndex&&) noexcept;
    OramaCoreFulltextIndex& operator=(OramaCoreFulltextIndex&&) noexcept;
    OramaCoreFulltextIndex(const OramaCoreFulltextIndex&) = delete;
    OramaCoreFulltextIndex& operator=(const OramaCoreFulltextIndex&) = delete;

    std::string name() const override { return std::string(kName); }
    IndexKind kind() const override { return IndexKind::Search; }

    // Is the oramacore-cpp engine compiled into this build? True iff
    // `ZETTA_WITH_ORAMACORE` was defined (the modules/oramacore-cpp submodule was
    // initialized + linked at build time). When false, build()/query() return
    // `Unsupported` so a CPU-only checkout that lacks the private submodule still
    // links + runs green — a test can branch on this.
    static bool engine_available();

    // Build (or rebuild) the BM25F full-text index described by `spec` over
    // `partitions`. Each indexed field becomes a BM25F field (default weight 1.0,
    // b 0.75); each row's `spec.fields` values are tokenized + indexed. A row
    // missing an indexed field, or carrying a non-text value for it, simply
    // contributes nothing for that field (tolerant over free-form row docs).
    // Returns Unsupported on a non-Search spec, InvalidArgument on an empty
    // `fields`, and (when the engine is not compiled in) Unsupported. Replaces any
    // index previously built under spec.key().
    IndexStatus build(const IndexSpec& spec,
                      std::span<const IndexPartition> partitions) override;

    // Run a full-text query: `query.term` (a text/scalar JSON value) is the
    // free-text query, scored by BM25F across all of the index's fields; the row
    // ids are returned **ranked by relevance** (score desc, then row id asc for a
    // stable, reproducible order), capped to `query.top_k` (0 = no cap). When set,
    // `query.field` must be one of the index's fields (else InvalidArgument), for
    // parity with the seam — scoring still spans all fields (BM25F weights them).
    // NotFound when no index was built under spec.key().
    IndexQueryResult query(const IndexSpec& spec,
                           const IndexQuery& query) const override;

    // Drop the index built under spec.key(). Idempotent (Ok even if none exists).
    IndexStatus drop(const IndexSpec& spec) override;

    // How many indexes are currently built (for tests / observability).
    std::size_t built_count() const;

private:
    struct Impl;                 // holds the oramacore-cpp engine state (pImpl)
    std::unique_ptr<Impl> impl_;
};

}  // namespace zetta
