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

// zetta's corpus statistics <-> the engine's (the same counts, keyed the same way).
oramacore::index::CorpusStats to_engine(const CorpusStatistics& statistics) {
    oramacore::index::CorpusStats out;
    out.doc_count = statistics.doc_count;
    for (const auto& [term, frequency] : statistics.term_doc_freq) {
        out.term_doc_freq.emplace(term, frequency);
    }
    for (const auto& [field, lengths] : statistics.fields) {
        out.fields.emplace(field, oramacore::index::FieldLengthStats{
                                      lengths.total_len, lengths.doc_count});
    }
    return out;
}

void add_from_engine(const oramacore::index::CorpusStats& statistics,
                     CorpusStatistics& out) {
    CorpusStatistics converted;
    converted.doc_count = statistics.doc_count;
    for (const auto& [term, frequency] : statistics.term_doc_freq) {
        converted.term_doc_freq.emplace(term, frequency);
    }
    for (const auto& [field, lengths] : statistics.fields) {
        converted.fields.emplace(
            field, FieldLengthStatistics{lengths.total_len, lengths.doc_count});
    }
    out.merge(converted);
}

// The checks query() and collect_statistics() share: a named field must be one
// the index covers, and the term must be text. Ok + the text on success.
IndexStatus validate_query(const IndexSpec& spec, const IndexQuery& query,
                           std::string& text) {
    if (!query.field.empty() &&
        std::find(spec.fields.begin(), spec.fields.end(), query.field) ==
            spec.fields.end()) {
        return IndexStatus::invalid_argument(
            "field '" + query.field + "' is not indexed by this index");
    }
    std::optional<std::string> scalar = scalar_text(query.term);
    if (!scalar) {
        return IndexStatus::invalid_argument(
            "full-text query term must be a text/scalar value");
    }
    text = std::move(*scalar);
    return IndexStatus::success();
}

// The engine index a query searches: the named field's own index when the query
// names one and the build kept one (lake#782), else the combined index. query()
// and collect_statistics() both use it, so a sharded field query merges and
// scores with that field's statistics alone. A template so it can take the
// private Impl::Built without naming it.
template <typename Built>
const oramacore::index::MemoryIndex& searched_index(const Built& built,
                                                    const IndexQuery& query) {
    if (!query.field.empty()) {
        if (const auto one = built.by_field.find(query.field);
            one != built.by_field.end()) {
            return one->second;
        }
    }
    return built.index;
}

}  // namespace

// One built BM25F index: the engine's in-memory index plus the mapping from its
// dense, insertion-ordered DocId back to zetta's stable row id (its `_zid`). The
// engine speaks DocId (a uint32); the IndexBackend interface speaks row id (a
// string) — this vector maps one to the other (rowid_by_docid[doc_id] == the row's id).
//
// The engine's search always scores across every field, so a query naming one
// field (lake#782) is answered by a second index holding only that field. Each
// per-field index is fed every row in the same order as the combined one, a row
// without the field as an empty document, so its DocIds map through the same
// rowid_by_docid. An index over one field needs none: the combined index is it.
struct OramaCoreFulltextIndex::Impl {
    struct Built {
        oramacore::index::MemoryIndex index;
        std::vector<std::string> rowid_by_docid;
        std::map<std::string, oramacore::index::MemoryIndex, std::less<>> by_field;
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

    Impl::Built built{oramacore::index::MemoryIndex(std::move(field_configs)), {}, {}};
    if (spec.fields.size() > 1) {
        for (const std::string& field : spec.fields) {
            built.by_field.try_emplace(field, std::vector<oramacore::index::FieldConfig>{
                                                  oramacore::index::FieldConfig{field, {}}});
        }
    }
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
            // so a hit's DocId maps straight back to its row id. Every per-field
            // index takes every row, so its ids stay aligned too.
            for (auto& [field, field_index] : built.by_field) {
                oramacore::index::Document field_doc;
                if (const auto text = engine_doc.find(field); text != engine_doc.end()) {
                    field_doc.emplace(field, text->second);
                }
                field_index.add(field_doc);
            }
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

    // A named field must be one this index covers, and then only that field is
    // searched (lake#782): a row matching in another field is not a hit.
    std::string text;
    result.status = validate_query(spec, query, text);
    if (!result.status.ok()) return result;

    const auto built = impl_->built.find(spec.key());
    if (built == impl_->built.end()) {
        result.status =
            IndexStatus::not_found("no index built for " + spec.key());
        return result;
    }

    // Score with the supplied corpus statistics when a sharded query passed them
    // (#851), else with this index's own.
    std::optional<oramacore::index::CorpusStats> global;
    if (query.statistics) global = to_engine(*query.statistics);
    // A query naming a field searches that field's own index (lake#782).
    const oramacore::index::MemoryIndex& searched = searched_index(built->second, query);
    const auto search = [&](std::size_t limit) {
        return global ? searched.search(text, limit, *global)
                      : searched.search(text, limit);
    };

    // The engine breaks a score tie by DocId (insertion order); this backend's
    // order, and the distributed merge's, breaks it by row id. So a cap must not
    // cut inside a tie the engine ordered: while the page is full and its last
    // hit still ties with the top_k-th, fetch a larger page, until the tie is
    // wholly inside it. Then sort by (score desc, row id asc) and cap.
    std::size_t limit = query.top_k;
    oramacore::index::SearchResult hits = search(limit);
    while (limit > 0 && hits.hits.size() == limit &&
           hits.hits.back().score == hits.hits[query.top_k - 1].score &&
           limit < built->second.rowid_by_docid.size()) {
        limit *= 2;
        hits = search(limit);
    }

    std::vector<std::pair<double, const std::string*>> ranked;
    ranked.reserve(hits.hits.size());
    for (const oramacore::index::Hit& hit : hits.hits) {
        if (hit.doc_id < built->second.rowid_by_docid.size()) {
            ranked.emplace_back(hit.score, &built->second.rowid_by_docid[hit.doc_id]);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return *a.second < *b.second;
    });
    if (query.top_k > 0 && ranked.size() > query.top_k) ranked.resize(query.top_k);

    result.row_ids.reserve(ranked.size());
    result.scores.reserve(ranked.size());
    for (const auto& [score, row_id] : ranked) {
        result.row_ids.push_back(*row_id);
        // Carry the BM25F relevance score (higher = better) aligned to
        // row_ids, so a distributed scatter-gather can merge per-partition
        // hits into one global ranking by score.
        result.scores.push_back(score);
    }
    result.status = IndexStatus::success();
    return result;
}

bool OramaCoreFulltextIndex::uses_corpus_statistics() const { return true; }

IndexStatus OramaCoreFulltextIndex::collect_statistics(
    const IndexSpec& spec, const IndexQuery& query, CorpusStatistics& out) const {
    std::string text;
    const IndexStatus valid = validate_query(spec, query, text);
    if (!valid.ok()) return valid;
    const auto built = impl_->built.find(spec.key());
    if (built == impl_->built.end()) {
        return IndexStatus::not_found("no index built for " + spec.key());
    }
    // The statistics of the index query() will search: a field's own index when
    // the query names one, so every shard merges and scores the same field.
    add_from_engine(searched_index(built->second, query).stats(text), out);
    return IndexStatus::success();
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

bool OramaCoreFulltextIndex::uses_corpus_statistics() const { return false; }

IndexStatus OramaCoreFulltextIndex::collect_statistics(const IndexSpec&,
                                                       const IndexQuery&,
                                                       CorpusStatistics&) const {
    return IndexStatus::unsupported(kEngineMissing);
}

std::size_t OramaCoreFulltextIndex::built_count() const { return 0; }

#endif  // ZETTA_WITH_ORAMACORE

}  // namespace zetta::index