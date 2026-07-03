#include "zetta/index/search.hpp"

#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/node.hpp"

namespace zetta::index {
namespace {

// A spec names its source table by NAME; partitions record the table's ID.
// Resolve name -> id across the node's databases (first match), falling back to
// the name itself (mirrors the build path in distributed_fulltext.cpp).
std::string resolve_table_id(Node& node, const std::string& table) {
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(name);
        if (!database) continue;
        if (const std::shared_ptr<zetta::data::Table> found =
                database->find_table(table)) {
            return found->id();
        }
    }
    return table;
}

// Resolve `target` to a registered Index entity: an exact index id wins;
// otherwise the most recently created Search-kind index over the table named
// `target` (indexes() is sorted (created, id), so the last match is the newest).
std::shared_ptr<Index> resolve_target(Node& node, const std::string& target) {
    if (std::shared_ptr<Index> by_id = node.find_index(target)) return by_id;
    std::shared_ptr<Index> newest;
    for (const std::shared_ptr<Index>& index : node.indexes()) {
        if (index && index->kind() == IndexKind::Search &&
            index->table() == target) {
            newest = index;
        }
    }
    return newest;
}

// Does the table have at least one PLACED partition (a body some node owns)?
// Placed partitions make the per-partition distributed path authoritative; a
// table with none degrades to the consolidated registry backend.
bool has_placed_partitions(Node& node, const std::string& table_id) {
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (partition && partition->table() == table_id &&
            !partition->blob_ref().empty()) {
            return true;
        }
    }
    return false;
}

}  // namespace

SearchResult search(Node& node, const SearchRequest& request,
                    zetta::observability::SpanRecorder* recorder,
                    const zetta::observability::TraceContext* parent) {
    SearchResult result;

    // The whole search is ONE trace: a root `search` span (under the caller's
    // trace when one is propagated, else a fresh root) with per-contribution
    // children below it.
    const zetta::observability::TraceContext trace =
        (parent && parent->valid()) ? *parent
                                    : zetta::observability::make_root_trace();
    zetta::observability::ScopedSpan root_span(recorder, trace, "search");
    result.trace_id = trace.trace_id;
    root_span.set_attribute(
        "intent", nlohmann::json{{"target", request.target},
                                 {"query", request.query},
                                 {"field", request.field},
                                 {"top_k", request.top_k}});

    const auto fail = [&](IndexStatus status) {
        result.status = std::move(status);
        result.duration_ms = root_span.elapsed_ms();
        root_span.finish(/*ok=*/false, result.status.message);
        return result;
    };

    if (request.target.empty()) {
        return fail(IndexStatus::invalid_argument(
            "search: target is empty (name an index id or a table)"));
    }
    if (request.query.empty()) {
        return fail(IndexStatus::invalid_argument("search: query is empty"));
    }

    // (1) Resolve the registered Index entity.
    const std::shared_ptr<Index> index = resolve_target(node, request.target);
    if (!index) {
        return fail(IndexStatus::not_found(
            "search: no index id or Search-kind index over a table named '" +
            request.target + "' is registered (create one with create_index)"));
    }
    if (index->kind() != IndexKind::Search) {
        return fail(IndexStatus::unsupported(
            "search: index '" + index->id() + "' is kind '" +
            std::string(index->kind_name()) +
            "' — only Search-kind indexes are searchable here"));
    }
    result.index_id = index->id();
    result.spec = index->spec();

    IndexQuery query;
    query.field = request.field;
    query.term = request.query;
    query.top_k = request.top_k;

    // (2) Route: placed partitions on a clustered node -> the distributed
    // per-partition path (the distributed-first default); otherwise the
    // consolidated registry backend (the degraded path).
    DistributedFulltextIndex* distributed = node.distributed_fulltext();
    const bool placed =
        has_placed_partitions(node, resolve_table_id(node, result.spec.table));
    const zetta::observability::TraceContext inner{trace.trace_id,
                                                   root_span.span_id()};

    if (placed && distributed) {
        result.path = SearchPath::Distributed;
        // Make this node's own sub-indexes current before answering (peers keep
        // theirs current the same way, from their tick).
        node.reconcile_search_indexes();
        const DistributedFulltextIndex::QueryResult distributed_result =
            distributed->query(result.spec, query, request.timeout, recorder,
                               &inner);
        if (!distributed_result.ok()) return fail(distributed_result.status);
        result.hits.reserve(distributed_result.row_ids.size());
        for (std::size_t i = 0; i < distributed_result.row_ids.size(); ++i) {
            result.hits.push_back(SearchHit{distributed_result.row_ids[i],
                                            distributed_result.scores[i]});
        }
        result.nodes = distributed_result.contributions;
        result.wire_bytes_moved = distributed_result.wire_bytes_moved;
    } else {
        result.path = SearchPath::Consolidated;
        const std::shared_ptr<IndexBackend> backend =
            node.index_registry().resolve(result.spec);
        if (!backend) {
            return fail(IndexStatus::not_found(
                "search: no registered backend named '" + result.spec.backend +
                "' implements kind 'search' on this node"));
        }
        zetta::observability::ScopedSpan local_span(recorder, inner,
                                                    "search:local");
        local_span.set_attribute("node_id", node.node_id());
        const IndexQueryResult query_result = backend->query(result.spec, query);
        if (!query_result.status.ok()) {
            local_span.finish(/*ok=*/false, query_result.status.message);
            return fail(query_result.status);
        }
        result.hits.reserve(query_result.row_ids.size());
        for (std::size_t i = 0; i < query_result.row_ids.size(); ++i) {
            const double score =
                i < query_result.scores.size() ? query_result.scores[i] : 0.0;
            result.hits.push_back(SearchHit{query_result.row_ids[i], score});
        }
        NodeContribution self;
        self.node_id = node.node_id();
        self.local = true;
        self.hits = result.hits.size();
        self.latency_ms = local_span.elapsed_ms();
        local_span.set_attribute("hits", self.hits);
        local_span.finish(/*ok=*/true);
        result.nodes.push_back(std::move(self));
    }

    // (3) The engine's plan + result shape, on the root span (observability.md).
    nlohmann::json participants = nlohmann::json::array();
    for (const NodeContribution& contribution : result.nodes) {
        participants.push_back(contribution.node_id);
    }
    root_span.set_attribute("plan",
                            nlohmann::json{{"path", to_string(result.path)},
                                           {"index_id", result.index_id},
                                           {"table", result.spec.table},
                                           {"backend", result.spec.backend},
                                           {"participants", participants}});
    root_span.set_attribute(
        "result_shape",
        nlohmann::json{{"hits", result.hits.size()},
                       {"nodes_answered", result.nodes.size()},
                       {"wire_bytes_moved", result.wire_bytes_moved}});
    result.status = IndexStatus::success();
    result.duration_ms = root_span.elapsed_ms();
    root_span.finish(/*ok=*/true);
    return result;
}

}  // namespace zetta::index
