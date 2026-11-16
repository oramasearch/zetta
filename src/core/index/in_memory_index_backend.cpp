// SPDX-License-Identifier: MIT

#include "zetta/index/in_memory_index_backend.hpp"

#include <algorithm>
#include <utility>

namespace zetta::index {

namespace {

// A stable string key for a scalar JSON value, used both when indexing a row's
// field value (build) and when matching a query term — rendering both sides the
// same way is what makes equality work. dump() gives `"abc"` for a string, `42`
// for an integer, `true` for a bool, so distinct scalars never collide. Non-scalar
// values (object / array / null) are not indexable for exact match — the caller
// guards those out.
std::string value_key(const nlohmann::json& value) { return value.dump(); }

// Is this field value indexable as an exact-match term? Strings, numbers, and
// booleans are; null and composite values are not.
bool indexable(const nlohmann::json& value) {
    return value.is_string() || value.is_number() || value.is_boolean();
}

}  // namespace

IndexStatus InMemoryIndexBackend::build(
    const IndexSpec& spec, std::span<const IndexPartition> partitions) {
    if (spec.kind != IndexKind::Search) {
        return IndexStatus::unsupported(
            "memory_search builds only 'search' indexes, not '" +
            std::string(to_string(spec.kind)) + "'");
    }
    if (spec.fields.empty()) {
        return IndexStatus::invalid_argument(
            "index spec names no fields to index");
    }

    BuiltIndex built;
    for (const IndexPartition& partition : partitions) {
        for (const IndexDocument& document : partition.documents) {
            if (!document.doc.is_object()) continue;  // tolerant over free-form rows
            for (const std::string& field : spec.fields) {
                const auto value = document.doc.find(field);
                if (value == document.doc.end() || !indexable(*value)) continue;
                built.postings[field][value_key(*value)].push_back(document.id);
            }
        }
    }
    // Deterministic posting lists: sorted, with duplicate ids (a row appearing in
    // two partitions, or the same value twice) collapsed.
    for (auto& [field, by_value] : built.postings) {
        for (auto& [value, ids] : by_value) {
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
    }

    indexes_[spec.key()] = std::move(built);  // replace any prior build
    return IndexStatus::success();
}

IndexQueryResult InMemoryIndexBackend::query(const IndexSpec& spec,
                                             const IndexQuery& query) const {
    IndexQueryResult result;

    // The queried field must be one this index covers.
    if (std::find(spec.fields.begin(), spec.fields.end(), query.field) ==
        spec.fields.end()) {
        result.status = IndexStatus::invalid_argument(
            "field '" + query.field + "' is not indexed by this index");
        return result;
    }

    const auto built = indexes_.find(spec.key());
    if (built == indexes_.end()) {
        result.status =
            IndexStatus::not_found("no index built for " + spec.key());
        return result;
    }

    // Look up the posting list for (field, term). A field with no scalar values
    // indexed, or a term with no matches, yields an empty (but successful) result.
    const auto by_value = built->second.postings.find(query.field);
    if (by_value != built->second.postings.end()) {
        const auto ids = by_value->second.find(value_key(query.term));
        if (ids != by_value->second.end()) {
            result.row_ids = ids->second;  // already sorted + deduped at build
            if (query.top_k > 0 && result.row_ids.size() > query.top_k) {
                result.row_ids.resize(query.top_k);
            }
        }
    }
    result.status = IndexStatus::success();
    return result;
}

IndexStatus InMemoryIndexBackend::drop(const IndexSpec& spec) {
    indexes_.erase(spec.key());  // idempotent: absent is fine
    return IndexStatus::success();
}

}  // namespace zetta::index