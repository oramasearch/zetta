#include "zetta/index/search.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/connect/connection_store.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/node.hpp"
#include "zetta/query/executor.hpp"
#include "zetta/storage/engine.hpp"

namespace zetta::index {

namespace {

constexpr std::string_view kAmbiguousSource = "several databases hold a table named";

IndexSource resolved_in(const std::shared_ptr<zetta::data::Database>& database,
                        const std::string& table, const std::string& missing) {
    IndexSource source;
    if (database) source.table = database->find_table(table);
    if (!source.table) {
        source.reason = missing;
        return source;
    }
    source.database = database->name();
    source.database_id = database->id();
    return source;
}

}  // namespace

IndexSource resolve_index_source(Node& node, const IndexSpec& spec,
                                 const std::string& database_name) {
    if (!spec.database_id.empty()) {
        return resolved_in(node.find_database_by_id(spec.database_id), spec.table,
                           "table '" + spec.table +
                               "' is not in the database the index records");
    }
    if (!database_name.empty()) {
        return resolved_in(node.find_database(database_name), spec.table,
                           "table '" + spec.table + "' is not in database '" +
                               database_name + "'");
    }
    IndexSource source;
    // A record from before the database was persisted: accept it only when the
    // name is unambiguous, so it can never bind to another database's table.
    std::vector<std::shared_ptr<zetta::data::Database>> holders;
    for (const std::string& name : node.database_names()) {
        std::shared_ptr<zetta::data::Database> database = node.find_database(name);
        if (database && database->find_table(spec.table)) {
            holders.push_back(std::move(database));
        }
    }
    if (holders.empty()) {
        source.reason = "table '" + spec.table + "' not found";
        return source;
    }
    if (holders.size() > 1) {
        std::string names;
        for (const auto& database : holders) {
            if (!names.empty()) names += ", ";
            names += database->name();
        }
        source.reason = "the index records no database and " +
                        std::string(kAmbiguousSource) + " '" + spec.table +
                        "' (" + names +
                        "); recreate it with create_index naming `database`";
        return source;
    }
    return resolved_in(holders.front(), spec.table,
                       "table '" + spec.table + "' not found");
}

bool ambiguous_index_source(const IndexSource& source) {
    return !source.ok() && source.reason.find(kAmbiguousSource) != std::string::npos;
}

// Resolve `target` to a registered Index entity: an exact index id wins;
// otherwise the most recently created Search-kind index over the table named
// `target` (indexes() is sorted (created, id), so the last match is the newest).
// Public (search.hpp) so the governed serving surface and expose_index apply
// THE same rule as search() itself — no second resolution path (#648).
std::shared_ptr<Index> resolve_search_target(Node& node,
                                             const std::string& target) {
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

namespace {

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

// The consolidated path's row lookup (#1243): the index was built over the
// table's own store (the index step executor), so its rows are on this node.
// Reads only the resolved source table (#1428). Removes missing or
// unauthorized rows before filling requested fields.
void attach_consolidated_fields(const zetta::data::Table& source,
                                const std::vector<std::string>& fields,
                                std::vector<SearchHit>& hits,
                                const zetta::query::ReadFilter& filter) {
    if (hits.empty()) return;
    std::unordered_set<std::string> allowed;
    std::unordered_map<std::string, SearchHit*> wanted;
    for (SearchHit& hit : hits) wanted.emplace(hit.row_id, &hit);
    {
        const zetta::storage::StorageEngine& store = source.store();
        for (std::size_t i = 0; i < store.size() && !wanted.empty(); ++i) {
            const auto hit = wanted.find(store.id_at(i));
            if (hit == wanted.end()) continue;
            if (const zetta::storage::ParquetLazyPtr row = store.at(i);
                row && row->is_object()) {
                if (!filter.allows(*row)) {
                    wanted.erase(hit);
                    continue;
                }
                allowed.insert(hit->second->row_id);
                for (const std::string& field : fields) {
                    if (const auto value = row->find(field); value != row->end()) {
                        hit->second->fields[field] = *value;
                    }
                }
            }
            wanted.erase(hit);
        }
    }
    std::erase_if(hits, [&](const SearchHit& hit) {
        return !allowed.contains(hit.row_id);
    });
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
    const std::shared_ptr<Index> index =
        resolve_search_target(node, request.target);
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

    // Which fields each hit carries back (#1243): the index's own by default,
    // and only ever fields the index holds.
    const std::vector<std::string> return_fields =
        request.fields ? *request.fields : result.spec.fields;
    for (const std::string& field : return_fields) {
        if (std::ranges::find(result.spec.fields, field) == result.spec.fields.end()) {
            std::string available;
            for (const std::string& indexed : result.spec.fields) {
                if (!available.empty()) available += ", ";
                available += indexed;
            }
            return fail(IndexStatus::invalid_argument(
                "search: field '" + field + "' is not one of index '" +
                index->id() + "''s fields (" + available + ")"));
        }
    }

    if (!request.field.empty() &&
        std::ranges::find(result.spec.fields, request.field) ==
            result.spec.fields.end()) {
        return fail(IndexStatus::invalid_argument(
            "search: query field is not indexed"));
    }

    // Resolve once, by the rule the build uses (#1428): the index's recorded
    // database, or the one database holding the name for an older record.
    // Authorization, routing and hydration all apply to that table, never a
    // different same-named one (#1403).
    const IndexSource source = resolve_index_source(node, result.spec);
    if (!source.ok()) {
        return fail(IndexStatus::not_found(
            "search: source table is unavailable: " + source.reason));
    }
    const std::string& database_name = source.database;
    // SQL exposes system views instead of stored system tables, and never
    // exposes the reserved connection store, even to an administrator.
    if (database_name == "system" ||
        database_name == zetta::connect::kConnectionDatabase) {
        return fail(IndexStatus::invalid_argument(
            "search: permission denied: source database is reserved by SQL"));
    }
    const zetta::governance::Operation read{
        zetta::governance::CapabilityDomain::Data, "read"};
    auto access = node.column_scope(request.acting_id, request.acting_is_user,
                                    read, database_name, result.spec.table);
    // Legacy keys must also hold the data right (column_scope's legacy contract
    // is being tightened independently in #1361).
    if (!request.acting_id.empty() && !request.acting_is_user &&
        !node.authorize(request.acting_id, read))
        access.table_allowed = false;
    if (!access.table_allowed) {
        return fail(IndexStatus::invalid_argument(
            "search: permission denied: no sql:read grant"));
    }
    for (const std::string& field : result.spec.fields) {
        if (!access.allows(field)) {
            return fail(IndexStatus::invalid_argument(
                "search: permission denied: query uses an unreadable column"));
        }
    }
    const zetta::query::ReadFilter filter = zetta::query::read_filter(
        node, request.acting_id, request.acting_is_user, database_name,
        result.spec.table);

    IndexQuery query;
    query.field = request.field;
    query.term = request.query;
    // Apply the caller's cap only after removing rows it cannot read.
    query.top_k = request.top_k;

    // (2) Route: placed partitions on a clustered node -> the distributed
    // per-partition path (the distributed-first default); otherwise the
    // consolidated registry backend (the degraded path).
    DistributedFulltextIndex* distributed = node.distributed_fulltext();
    const bool placed = has_placed_partitions(node, source.table->id());
    const zetta::observability::TraceContext inner{trace.trace_id,
                                                   root_span.span_id()};

    if (placed && distributed) {
        result.path = SearchPath::Distributed;
        // The indexed partition body can retain an old owner after a raw-row
        // update. Until hits can be checked against current authoritative rows,
        // refusing an RLS search prevents stale policy operands from widening it.
        if (!filter.unrestricted) {
            return fail(IndexStatus::invalid_argument(
                "search: row-level security is enabled; distributed search is unavailable"));
        }
        // Make this node's own sub-indexes current before answering (peers keep
        // theirs current the same way, from their tick).
        node.reconcile_search_indexes();
        const DistributedFulltextIndex::QueryResult distributed_result =
            distributed->query(result.spec, query, request.timeout, recorder,
                               &inner,
                               return_fields.empty() ? nullptr : &return_fields,
                               &database_name);
        if (!distributed_result.ok()) return fail(distributed_result.status);
        result.hits.reserve(distributed_result.row_ids.size());
        for (std::size_t i = 0; i < distributed_result.row_ids.size(); ++i) {
            SearchHit hit{distributed_result.row_ids[i], distributed_result.scores[i]};
            // The owning node attached the row's values to its own hits.
            if (i < distributed_result.fields.size() &&
                distributed_result.fields[i].is_object()) {
                hit.fields = distributed_result.fields[i];
            }
            result.hits.push_back(std::move(hit));
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
        IndexQuery uncapped = query;
        if (!filter.unrestricted) uncapped.top_k = 0;
        const IndexQueryResult query_result =
            backend->query(result.spec, uncapped);
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
        attach_consolidated_fields(*source.table, return_fields, result.hits,
                                   filter);
        NodeContribution self;
        self.node_id = node.node_id();
        self.local = true;
        self.hits = result.hits.size();
        self.sub_indexes = 1;  // the one consolidated index answered
        self.latency_ms = local_span.elapsed_ms();
        local_span.set_attribute("hits", self.hits);
        local_span.finish(/*ok=*/true);
        result.nodes.push_back(std::move(self));
    }

    if (request.top_k && result.hits.size() > request.top_k) {
        result.hits.resize(request.top_k);
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
