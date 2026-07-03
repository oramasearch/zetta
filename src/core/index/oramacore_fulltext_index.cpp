#include "zetta/index/oramacore_fulltext_index.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(ZETTA_WITH_ORAMACORE) && ZETTA_WITH_ORAMACORE
#include "oramacore/index.hpp"
#endif

namespace zetta::index {

#if defined(ZETTA_WITH_ORAMACORE) && ZETTA_WITH_ORAMACORE

namespace {

// Render a row's free-form JSON field value to the text the engine tokenizes.
// Strings pass through; numbers/booleans stringify to their bare token form
// (`42`, `true`) so a numeric/boolean field is still searchable as a token;
// null / arrays / objects are not text and contribute nothing (std::nullopt).
std::optional<std::string> scalar_text(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number() || value.is_boolean()) return value.dump();
    return std::nullopt;
}

}  // namespace

// One built BM25F index: the engine's in-memory index plus the mapping from its
// dense, insertion-ordered DocId back to zetta's stable row id (its `_zid`). The
// engine speaks DocId (a uint32); the seam speaks row id (a string) — this vector
// is the bridge (rowid_by_docid[doc_id] == the row's id).
struct OramaCoreFulltextIndex::Impl {
    struct Built {
        oramacore::index::MemoryIndex index;
        std::vector<std::string> rowid_by_docid;
    };
    std::map<std::string, Built> built;  // spec.key() -> built index
};

OramaCoreFulltextIndex::OramaCoreFulltextIndex()
    : impl_(std::make_unique<Impl>()) {}
OramaCoreFulltextIndex::~OramaCoreFulltextIndex() = default;
OramaCoreFulltextIndex::OramaCoreFulltextIndex(OramaCoreFulltextIndex&&) noexcept =
    default;
OramaCoreFulltextIndex& OramaCoreFulltextIndex::operator=(
    OramaCoreFulltextIndex&&) noexcept = default;

bool OramaCoreFulltextIndex::engine_available() { return true; }

IndexStatus OramaCoreFulltextIndex::build(
    const IndexSpec& spec, std::span<const IndexPartition> partitions) {
    if (spec.kind != IndexKind::Search) {
        return IndexStatus::unsupported(
            "oramacore_fulltext builds only 'search' (full-text) indexes, not '" +
            std::string(to_string(spec.kind)) + "'");
    }
    if (spec.fields.empty()) {
        return IndexStatus::invalid_argument(
            "index spec names no fields to index");
    }

    // One BM25F field per indexed field, default params (weight 1.0, b 0.75).
    std::vector<oramacore::index::FieldConfig> field_configs;
    field_configs.reserve(spec.fields.size());
    for (const std::string& field : spec.fields) {
        field_configs.push_back(oramacore::index::FieldConfig{field, {}});
    }

    Impl::Built built{oramacore::index::MemoryIndex(std::move(field_configs)), {}};
    // Index each row id once. Partitions are disjoint chunks of the table, but
    // guard a row appearing in two partitions (the tolerance InMemoryIndexBackend
    // gives) so it can't be double-counted into its BM25F term frequencies or
    // listed twice in a result.
    std::unordered_set<std::string> seen_ids;
    for (const IndexPartition& partition : partitions) {
        for (const IndexDocument& document : partition.documents) {
            if (!seen_ids.insert(document.id).second) continue;  // already indexed
            oramacore::index::Document engine_doc;
            if (document.doc.is_object()) {  // tolerant over free-form rows
                for (const std::string& field : spec.fields) {
                    const auto value = document.doc.find(field);
                    if (value == document.doc.end()) continue;
                    if (auto text = scalar_text(*value)) {
                        engine_doc.emplace(field, std::move(*text));
                    }
                }
            }
            // add() assigns dense ids in call order; keep rowid_by_docid aligned
            // so a hit's DocId maps straight back to its row id.
            const auto doc_id = built.index.add(engine_doc);
            if (doc_id >= built.rowid_by_docid.size()) {
                built.rowid_by_docid.resize(doc_id + 1);
            }
            built.rowid_by_docid[doc_id] = document.id;
        }
    }

    impl_->built.insert_or_assign(spec.key(), std::move(built));  // replace prior
    return IndexStatus::success();
}

IndexQueryResult OramaCoreFulltextIndex::query(const IndexSpec& spec,
                                               const IndexQuery& query) const {
    IndexQueryResult result;

    // A named field must be one this index covers (parity with the seam). BM25F
    // scores across all fields regardless — the field just validates here.
    if (!query.field.empty() &&
        std::find(spec.fields.begin(), spec.fields.end(), query.field) ==
            spec.fields.end()) {
        result.status = IndexStatus::invalid_argument(
            "field '" + query.field + "' is not indexed by this index");
        return result;
    }

    const auto text = scalar_text(query.term);
    if (!text) {
        result.status = IndexStatus::invalid_argument(
            "full-text query term must be a text/scalar value");
        return result;
    }

    const auto built = impl_->built.find(spec.key());
    if (built == impl_->built.end()) {
        result.status =
            IndexStatus::not_found("no index built for " + spec.key());
        return result;
    }

    // Rank by BM25F relevance. The engine already orders hits score-desc with a
    // stable DocId tiebreak; mapping each hit's DocId back to its row id preserves
    // that deterministic, reproducible order.
    const oramacore::index::SearchResult hits =
        built->second.index.search(*text, query.top_k);
    result.row_ids.reserve(hits.hits.size());
    result.scores.reserve(hits.hits.size());
    for (const oramacore::index::Hit& hit : hits.hits) {
        if (hit.doc_id < built->second.rowid_by_docid.size()) {
            result.row_ids.push_back(built->second.rowid_by_docid[hit.doc_id]);
            // Carry the BM25F relevance score (higher = better) aligned to
            // row_ids, so a distributed scatter-gather can merge per-partition
            // hits into one global ranking by score.
            result.scores.push_back(hit.score);
        }
    }
    result.status = IndexStatus::success();
    return result;
}

IndexStatus OramaCoreFulltextIndex::drop(const IndexSpec& spec) {
    impl_->built.erase(spec.key());  // idempotent: absent is fine
    return IndexStatus::success();
}

std::size_t OramaCoreFulltextIndex::built_count() const {
    return impl_->built.size();
}

#else  // !ZETTA_WITH_ORAMACORE — the engine was not compiled into this build.

// The submodule was not initialized / linked at build time (e.g. a CPU-only CI
// checkout without access to the private oramacore-cpp repo). The backend stays a
// usable, registerable IndexBackend, but build()/query() report that the engine
// is missing rather than silently doing nothing — so a caller can route elsewhere
// (the IndexStatusCode::Unsupported contract), and the build still links + runs.
struct OramaCoreFulltextIndex::Impl {};

namespace {
const std::string kEngineMissing =
    "oramacore-cpp full-text engine is not compiled into this build (the "
    "modules/oramacore-cpp submodule was not initialized at build time); rebuild "
    "with the submodule present to use the full-text backend";
}  // namespace

OramaCoreFulltextIndex::OramaCoreFulltextIndex()
    : impl_(std::make_unique<Impl>()) {}
OramaCoreFulltextIndex::~OramaCoreFulltextIndex() = default;
OramaCoreFulltextIndex::OramaCoreFulltextIndex(OramaCoreFulltextIndex&&) noexcept =
    default;
OramaCoreFulltextIndex& OramaCoreFulltextIndex::operator=(
    OramaCoreFulltextIndex&&) noexcept = default;

bool OramaCoreFulltextIndex::engine_available() { return false; }

IndexStatus OramaCoreFulltextIndex::build(const IndexSpec&,
                                          std::span<const IndexPartition>) {
    return IndexStatus::unsupported(kEngineMissing);
}

IndexQueryResult OramaCoreFulltextIndex::query(const IndexSpec&,
                                               const IndexQuery&) const {
    return IndexQueryResult{IndexStatus::unsupported(kEngineMissing), {}};
}

IndexStatus OramaCoreFulltextIndex::drop(const IndexSpec&) {
    return IndexStatus::success();  // nothing built — the post-condition holds
}

std::size_t OramaCoreFulltextIndex::built_count() const { return 0; }

#endif  // ZETTA_WITH_ORAMACORE

}  // namespace zetta::index