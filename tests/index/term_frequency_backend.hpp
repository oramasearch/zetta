#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/index/distributed_fulltext.hpp"  // FulltextHit
#include "zetta/index/index_backend.hpp"

// A deterministic, partition-INDEPENDENT reference scorer shared by the
// distributed-fulltext tests (test_distributed_fulltext.cpp) and the
// Node::search entry-point tests (test_node_search.cpp).
//
// A doc's score for a query is its term frequency — the count of query tokens
// occurring in the doc's indexed text — which depends ONLY on the doc + the
// query, never on corpus statistics. That is what makes "build per-partition
// then merge" equal to "build one consolidated index" exactly, so it is a clean
// oracle for the distributed merge without the private BM25F engine. (Real
// BM25F is NOT partition-independent — its IDF depends on the corpus — which is
// why the real-engine acceptances use a corpus whose per-partition statistics
// are engineered to equal the consolidated ones; see postgres_corpus in
// test_distributed_fulltext.cpp.)
namespace zetta::test {

inline std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            current.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

class TermFrequencyBackend final : public zetta::index::IndexBackend {
public:
    std::string name() const override { return "tf_reference"; }
    zetta::index::IndexKind kind() const override {
        return zetta::index::IndexKind::Search;
    }

    zetta::index::IndexStatus build(
        const zetta::index::IndexSpec& spec,
        std::span<const zetta::index::IndexPartition> partitions) override {
        if (spec.kind != zetta::index::IndexKind::Search) {
            return zetta::index::IndexStatus::unsupported(
                "tf_reference is search-only");
        }
        if (spec.fields.empty()) {
            return zetta::index::IndexStatus::invalid_argument("no fields");
        }
        Built built;
        for (const zetta::index::IndexPartition& partition : partitions) {
            for (const zetta::index::IndexDocument& document :
                 partition.documents) {
                if (!document.doc.is_object()) continue;
                std::map<std::string, int> counts;
                for (const std::string& field : spec.fields) {
                    const auto value = document.doc.find(field);
                    if (value == document.doc.end() || !value->is_string()) {
                        continue;
                    }
                    for (const std::string& token :
                         tokenize(value->get<std::string>())) {
                        ++counts[token];
                    }
                }
                built.docs.push_back({document.id, std::move(counts)});
            }
        }
        indexes_[spec.key()] = std::move(built);
        return zetta::index::IndexStatus::success();
    }

    zetta::index::IndexQueryResult query(
        const zetta::index::IndexSpec& spec,
        const zetta::index::IndexQuery& query) const override {
        zetta::index::IndexQueryResult result;
        const auto found = indexes_.find(spec.key());
        if (found == indexes_.end()) {
            result.status =
                zetta::index::IndexStatus::not_found("no index for " + spec.key());
            return result;
        }
        const std::vector<std::string> query_tokens =
            tokenize(query.term.is_string() ? query.term.get<std::string>()
                                            : query.term.dump());
        std::vector<zetta::index::FulltextHit> hits;
        for (const Doc& doc : found->second.docs) {
            double score = 0.0;
            for (const std::string& token : query_tokens) {
                const auto count = doc.counts.find(token);
                if (count != doc.counts.end()) score += count->second;
            }
            if (score > 0.0) hits.push_back({doc.id, score});
        }
        std::stable_sort(hits.begin(), hits.end(),
                         [](const zetta::index::FulltextHit& a,
                            const zetta::index::FulltextHit& b) {
                             if (a.score != b.score) return a.score > b.score;
                             return a.row_id < b.row_id;
                         });
        if (query.top_k > 0 && hits.size() > query.top_k) {
            hits.resize(query.top_k);
        }
        for (const zetta::index::FulltextHit& hit : hits) {
            result.row_ids.push_back(hit.row_id);
            result.scores.push_back(hit.score);
        }
        result.status = zetta::index::IndexStatus::success();
        return result;
    }

    zetta::index::IndexStatus drop(const zetta::index::IndexSpec& spec) override {
        indexes_.erase(spec.key());
        return zetta::index::IndexStatus::success();
    }

private:
    struct Doc {
        std::string id;
        std::map<std::string, int> counts;  // token -> frequency in the doc
    };
    struct Built {
        std::vector<Doc> docs;
    };
    std::map<std::string, Built> indexes_;
};

}  // namespace zetta::test
