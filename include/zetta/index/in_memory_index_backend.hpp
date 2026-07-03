#pragma once

#include <map>
#include <span>
#include <string>
#include <vector>

#include "zetta/index/index_backend.hpp"

// The reference IndexBackend (ENT-39 / ENT-40, ADR-34) — a from-scratch, in-memory
// **search** backend that makes the seam end-to-end usable and gives future
// vector / semantic backends a concrete template to mirror (the same role
// compute::HostStubDevice / CpuDevice play for the ComputeDevice seam). It lives
// in zetta-core (pure host C++ — no SDL/CUDA/heavy dependency).
//
// It implements IndexKind::Search as an **inverted index**: for each indexed
// field, build(map) value → the row ids carrying that value, across all
// partitions. query() returns the row ids whose `field` equals `term`. Real
// search (tokenization, ranking, on-disk segments) and the vector / feature
// backends are later slices behind the same seam; this backend is correctness +
// shape, not a production search engine.
namespace zetta::index {

class InMemoryIndexBackend final : public IndexBackend {
public:
    // The backend's stable selector name — what an Index::backend / IndexSpec
    // names to resolve to this backend in an IndexRegistry.
    static constexpr std::string_view kName = "memory_search";

    std::string name() const override { return std::string(kName); }
    IndexKind kind() const override { return IndexKind::Search; }

    // Build the inverted index for `spec` over `partitions`. Rejects a spec whose
    // kind is not Search (Unsupported) or that names no fields (InvalidArgument).
    // A row missing an indexed field, or carrying a non-scalar value for it, is
    // skipped for that field (a tolerant build over free-form row docs). Replaces
    // any index previously built under spec.key().
    IndexStatus build(const IndexSpec& spec,
                      std::span<const IndexPartition> partitions) override;

    // Return the row ids whose `query.field` equals `query.term`, in ascending id
    // order (deterministic), capped to `query.top_k` (0 = no cap). NotFound when no
    // index was built under spec.key(); InvalidArgument when `query.field` is not
    // one of the spec's indexed fields.
    IndexQueryResult query(const IndexSpec& spec,
                           const IndexQuery& query) const override;

    // Drop the index built under spec.key(). Idempotent (Ok even if none exists).
    IndexStatus drop(const IndexSpec& spec) override;

    // How many indexes are currently built (for tests / observability).
    std::size_t built_count() const { return indexes_.size(); }

private:
    // One built index: per indexed field, a map from the field's value (rendered
    // to a stable string key) to the ids of the rows carrying it. A std::map keeps
    // values — and the ids within each posting list — in deterministic order.
    struct BuiltIndex {
        // field name -> (value-key -> sorted row ids)
        std::map<std::string, std::map<std::string, std::vector<std::string>>>
            postings;
    };

    std::map<std::string, BuiltIndex> indexes_;  // spec.key() -> built index
};

}  // namespace zetta::index