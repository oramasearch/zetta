#include "zetta/index/distributed_fulltext.hpp"

#include <algorithm>
#include <array>
#include <istream>
#include <sstream>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster.hpp"
#include "zetta/database.hpp"
#include "zetta/dataset_partition.hpp"
#include "zetta/index/oramacore_fulltext_index.hpp"
#include "zetta/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/table.hpp"

namespace zetta {
namespace {

// Separator that splits a sub-index's logical "table" into the base table + the
// partition id (so each per-partition sub-index gets a distinct IndexSpec::key()).
// The ASCII unit separator never appears in a table name or a partition id, so the
// derived key can't collide with a real table's key.
constexpr char kPartitionMarker = '\x1f';

// --- schema boundary (the collective-message pattern) ------------------------

// Collects the first validation failure without letting it cross the validator
// boundary as an exception (matching src/core/collectives.cpp).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

void set_error(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

// The fulltext.query REQUEST schema (draft-07). Mirrored at
// schemas/fulltext_query.schema.json; this embedded copy is the one enforced at
// runtime (the signal-envelope / collective-message pattern). A closed object: the
// index it names (table/kind/fields) + the query (term) are required; backend /
// field / tag / top_k are optional.
const nlohmann::json& query_schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.FulltextQueryRequest",
      "type": "object",
      "properties": {
        "tag":     { "type": "string" },
        "table":   { "type": "string", "minLength": 1 },
        "kind":    { "type": "string", "enum": ["search", "vector", "feature"] },
        "backend": { "type": "string" },
        "fields":  { "type": "array", "minItems": 1, "items": { "type": "string" } },
        "field":   { "type": "string" },
        "term":    { "type": ["string", "number", "boolean", "null"] },
        "top_k":   { "type": "integer", "minimum": 0 }
      },
      "required": ["table", "kind", "fields", "term"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// The fulltext.query RESPONSE schema (draft-07). Mirrored at
// schemas/fulltext_hits.schema.json. One node's ranked hits: a list of
// { id, score } objects.
const nlohmann::json& hits_schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.FulltextQueryResponse",
      "type": "object",
      "properties": {
        "from": { "type": "string" },
        "hits": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id":    { "type": "string" },
              "score": { "type": "number" }
            },
            "required": ["id", "score"],
            "additionalProperties": false
          }
        }
      },
      "required": ["hits"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& query_validator() {
    static nlohmann::json_schema::json_validator validator(query_schema_document());
    return validator;
}

nlohmann::json_schema::json_validator& hits_validator() {
    static nlohmann::json_schema::json_validator validator(hits_schema_document());
    return validator;
}

// --- partition-body reading (compute-near-data) ------------------------------

// Read the body of a large object by id from whichever of the node's databases
// holds it (its `blob_ref` is a globally-unique `lo:` id). nullopt when no database
// holds the body locally. Mirrors zetta::ddf's local-partition body read.
std::optional<std::string> read_blob_body(Node& node, const std::string& blob_ref) {
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<Database> database = node.find_database(name);
        if (!database) continue;
        if (std::optional<std::string> body =
                database->large_objects().read(blob_ref)) {
            return body;
        }
    }
    return std::nullopt;
}

// Resolve `table` (a name from the IndexSpec) to the `tbl:` id the partition catalog
// records, so owned partitions can be enumerated. Falls back to `table` unchanged
// when no table of that name is found (it may already be an id).
std::string resolve_table_id(Node& node, const std::string& table) {
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<Database> database = node.find_database(name);
        if (!database) continue;
        if (const std::shared_ptr<Table> found = database->find_table(table)) {
            return found->id();
        }
    }
    return table;
}

// Parse a partition body's JsonLines rows into IndexDocuments. The body is one
// `{"_zid": <id>, "doc": <row>}` object per line (the JsonlStore on-disk shape the
// partitioned write path emits) — read the same tolerant way JsonlStore::load does,
// but **fail fast** on a line that is not valid JSON or not a JSON object (a silent
// drop would diverge from the single-index oracle). Returns false + a reason on the
// first malformed line.
bool parse_body_documents(const std::string& body, const std::string& partition_id,
                          std::vector<IndexDocument>& documents,
                          std::uint64_t& rows, std::string* error) {
    std::istringstream in(body);
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        nlohmann::json parsed =
            nlohmann::json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            set_error(error, "partition '" + partition_id + "' line " +
                                 std::to_string(line_no) + " is not valid JSON");
            return false;
        }
        std::string id;
        nlohmann::json doc;
        if (parsed.is_object() && parsed.contains("_zid") && parsed.contains("doc")) {
            const nlohmann::json& zid = parsed["_zid"];
            id = zid.is_string() ? zid.get<std::string>()
                                 : partition_id + "#" + std::to_string(line_no);
            doc = std::move(parsed["doc"]);
        } else if (parsed.is_object()) {
            id = partition_id + "#" + std::to_string(line_no);
            doc = std::move(parsed);
        } else {
            set_error(error, "partition '" + partition_id + "' line " +
                                 std::to_string(line_no) + " is not a JSON object");
            return false;
        }
        documents.push_back(IndexDocument{std::move(id), std::move(doc)});
        ++rows;
    }
    return true;
}

}  // namespace

// --- the pure merge ----------------------------------------------------------

std::vector<FulltextHit> merge_fulltext_hits(
    std::span<const std::vector<FulltextHit>> sources, std::size_t top_k) {
    // Dedup by row id, keeping the highest score (a row id should appear in exactly
    // one partition, but a duplicate is collapsed rather than listed twice). std::map
    // gives ascending row-id order, the stable tiebreak.
    std::map<std::string, double> best;
    for (const std::vector<FulltextHit>& source : sources) {
        for (const FulltextHit& hit : source) {
            const auto [it, inserted] = best.try_emplace(hit.row_id, hit.score);
            if (!inserted && hit.score > it->second) it->second = hit.score;
        }
    }

    std::vector<FulltextHit> merged;
    merged.reserve(best.size());
    for (const auto& [row_id, score] : best) {
        merged.push_back(FulltextHit{row_id, score});
    }
    // Score descending; ascending row id breaks a tie (stable_sort over the already
    // id-ascending vector → deterministic, reproducible).
    std::stable_sort(merged.begin(), merged.end(),
                     [](const FulltextHit& a, const FulltextHit& b) {
                         if (a.score != b.score) return a.score > b.score;
                         return a.row_id < b.row_id;
                     });
    if (top_k > 0 && merged.size() > top_k) merged.resize(top_k);
    return merged;
}

// --- wire messages -----------------------------------------------------------

IndexSpec FulltextQueryRequest::spec() const {
    return IndexSpec{table, kind, backend, fields};
}

IndexQuery FulltextQueryRequest::query() const {
    IndexQuery q;
    q.field = field;
    q.term = term;
    q.top_k = top_k;
    return q;
}

std::string encode_fulltext_query(const FulltextQueryRequest& request) {
    nlohmann::json document;
    if (!request.tag.empty()) document["tag"] = request.tag;
    document["table"] = request.table;
    document["kind"] = std::string(to_string(request.kind));
    if (!request.backend.empty()) document["backend"] = request.backend;
    document["fields"] = request.fields;
    if (!request.field.empty()) document["field"] = request.field;
    document["term"] = request.term;
    document["top_k"] = request.top_k;
    return document.dump();
}

std::optional<FulltextQueryRequest> decode_fulltext_query(std::string_view payload,
                                                          std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "fulltext query is not valid JSON");
        return std::nullopt;
    }

    collecting_error_handler handler;
    query_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    FulltextQueryRequest request;
    request.tag = document.value("tag", std::string{});
    request.table = document.value("table", std::string{});
    const std::optional<IndexKind> kind =
        index_kind_from_name(document.value("kind", std::string{}));
    if (!kind) {  // schema enum guards this, but stay defensive
        set_error(error, "/kind: unknown index kind");
        return std::nullopt;
    }
    request.kind = *kind;
    request.backend = document.value("backend", std::string{});
    request.fields = document.at("fields").get<std::vector<std::string>>();
    request.field = document.value("field", std::string{});
    request.term = document.at("term");
    request.top_k = document.value("top_k", static_cast<std::size_t>(0));
    return request;
}

std::string encode_fulltext_hits(const FulltextQueryResponse& response) {
    nlohmann::json document;
    if (!response.from.empty()) document["from"] = response.from;
    nlohmann::json hits = nlohmann::json::array();
    for (const FulltextHit& hit : response.hits) {
        hits.push_back({{"id", hit.row_id}, {"score", hit.score}});
    }
    document["hits"] = std::move(hits);
    return document.dump();
}

std::optional<FulltextQueryResponse> decode_fulltext_hits(std::string_view payload,
                                                          std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "fulltext hits is not valid JSON");
        return std::nullopt;
    }

    collecting_error_handler handler;
    hits_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    FulltextQueryResponse response;
    response.from = document.value("from", std::string{});
    for (const nlohmann::json& hit : document.at("hits")) {
        response.hits.push_back(
            FulltextHit{hit.at("id").get<std::string>(), hit.at("score").get<double>()});
    }
    return response;
}

// --- DistributedFulltextIndex ------------------------------------------------

DistributedFulltextIndex::DistributedFulltextIndex(
    Cluster& cluster, std::shared_ptr<IndexBackend> backend, SignalHandler* delegate)
    : cluster_(cluster),
      backend_(backend ? std::move(backend)
                       : std::make_shared<OramaCoreFulltextIndex>()),
      delegate_(delegate) {
    cluster_.set_signal_handler(this);
}

DistributedFulltextIndex::~DistributedFulltextIndex() {
    // Restore the handler the node would otherwise run (the delegate, or none).
    cluster_.set_signal_handler(delegate_);
}

std::vector<std::string> DistributedFulltextIndex::participants() const {
    std::vector<std::string> ids;
    ids.push_back(cluster_.node_id());
    for (const ClusterPeer& peer : cluster_.peers()) ids.push_back(peer.node_id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

DistributedFulltextIndex::BuildResult DistributedFulltextIndex::build_local(
    Node& node, const IndexSpec& spec) {
    BuildResult out;
    const std::string& self = node.node_id();
    const std::string table_id = resolve_table_id(node, spec.table);

    // Enumerate this node's owned partitions of the table, ascending by row range so
    // the build is deterministic / reproducible (compute-near-data: only the bodies
    // this node physically holds).
    std::vector<std::shared_ptr<DatasetPartition>> owned;
    for (const std::shared_ptr<DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) continue;
        const std::vector<std::string>& owners = partition->owner_nodes();
        if (std::find(owners.begin(), owners.end(), self) == owners.end()) continue;
        owned.push_back(partition);
    }
    std::sort(owned.begin(), owned.end(),
              [](const std::shared_ptr<DatasetPartition>& a,
                 const std::shared_ptr<DatasetPartition>& b) {
                  return a->row_range().start < b->row_range().start;
              });

    const std::lock_guard<std::mutex> lock(mutex_);

    // Drop any sub-indexes previously built for this spec (rebuild semantics).
    const auto prior = sub_specs_.find(spec.key());
    if (prior != sub_specs_.end()) {
        for (const IndexSpec& sub : prior->second) backend_->drop(sub);
        sub_specs_.erase(prior);
    }

    std::vector<IndexSpec> subs;
    for (const std::shared_ptr<DatasetPartition>& partition : owned) {
        if (partition->blob_ref().empty()) continue;  // unplaced / empty partition
        const std::optional<std::string> body =
            read_blob_body(node, partition->blob_ref());
        if (!body) {
            out.status = IndexStatus::backend_error(
                "build_local: owned partition '" + partition->id() + "' body (" +
                partition->blob_ref() + ") is not present locally");
            return out;
        }
        out.local_bytes_read += body->size();

        IndexPartition built_partition;
        built_partition.id = partition->id();
        std::string parse_error;
        if (!parse_body_documents(*body, partition->id(),
                                  built_partition.documents, out.rows_indexed,
                                  &parse_error)) {
            out.status = IndexStatus::invalid_argument("build_local: " + parse_error);
            return out;
        }

        // One sub-index per partition, keyed distinctly by suffixing the table.
        IndexSpec sub = spec;
        sub.table = spec.table + kPartitionMarker + "part" + kPartitionMarker +
                    partition->id();
        const std::array<IndexPartition, 1> one{std::move(built_partition)};
        const IndexStatus status = backend_->build(sub, one);
        if (!status.ok()) {
            out.status = status;  // e.g. Unsupported when the engine is not built in
            return out;
        }
        subs.push_back(std::move(sub));
        ++out.partitions_indexed;
    }

    sub_specs_[spec.key()] = std::move(subs);
    out.status = IndexStatus::success();
    return out;
}

std::vector<FulltextHit> DistributedFulltextIndex::query_local(
    const IndexSpec& spec, const IndexQuery& query, IndexStatus* status) const {
    const std::lock_guard<std::mutex> lock(mutex_);

    const auto found = sub_specs_.find(spec.key());
    if (found == sub_specs_.end() || found->second.empty()) {
        // This node holds no partitions of the index — it contributes nothing (Ok).
        if (status) *status = IndexStatus::success();
        return {};
    }

    std::vector<std::vector<FulltextHit>> per_partition;
    per_partition.reserve(found->second.size());
    IndexStatus first_failure;  // Ok unless a sub-query reports otherwise
    for (const IndexSpec& sub : found->second) {
        const IndexQueryResult result = backend_->query(sub, query);
        if (!result.status.ok()) {
            if (first_failure.ok()) first_failure = result.status;
            continue;
        }
        std::vector<FulltextHit> hits;
        hits.reserve(result.row_ids.size());
        for (std::size_t i = 0; i < result.row_ids.size(); ++i) {
            const double score = i < result.scores.size() ? result.scores[i] : 0.0;
            hits.push_back(FulltextHit{result.row_ids[i], score});
        }
        per_partition.push_back(std::move(hits));
    }
    if (status) *status = first_failure;
    return merge_fulltext_hits(per_partition, query.top_k);
}

DistributedFulltextIndex::QueryResult DistributedFulltextIndex::query(
    const IndexSpec& spec, const IndexQuery& query,
    std::chrono::milliseconds timeout) const {
    QueryResult out;

    // (1) This node's local contribution (across its own partitions).
    IndexStatus local_status;
    std::vector<FulltextHit> local = query_local(spec, query, &local_status);
    if (!local_status.ok()) {
        out.status = local_status;
        return out;
    }

    std::vector<std::vector<FulltextHit>> sources;
    sources.push_back(std::move(local));
    std::size_t answered = 1;
    std::uint64_t wire = 0;

    // (2) Scatter the query to every connected peer and gather their ranked hits.
    FulltextQueryRequest request;
    request.table = spec.table;
    request.kind = spec.kind;
    request.backend = spec.backend;
    request.fields = spec.fields;
    request.field = query.field;
    request.term = query.term;
    request.top_k = query.top_k;
    const std::string payload = encode_fulltext_query(request);

    for (const ClusterPeer& peer : cluster_.peers()) {
        const SignalResponse response = cluster_.request(
            peer.node_id, std::string(kQueryMethod), payload, timeout);
        wire += payload.size();
        if (!response.ok) {
            out.status = IndexStatus::backend_error(
                "fulltext.query to peer '" + peer.node_id + "' failed: " +
                response.error);
            return out;
        }
        wire += response.payload.size();
        std::string decode_error;
        const std::optional<FulltextQueryResponse> hits =
            decode_fulltext_hits(response.payload, &decode_error);
        if (!hits) {
            out.status = IndexStatus::backend_error(
                "fulltext hits from peer '" + peer.node_id +
                "' are malformed: " + decode_error);
            return out;
        }
        sources.push_back(std::move(hits->hits));
        ++answered;
    }

    // (3) Merge every contribution into the global ranking.
    const std::vector<FulltextHit> merged =
        merge_fulltext_hits(sources, query.top_k);
    out.row_ids.reserve(merged.size());
    out.scores.reserve(merged.size());
    for (const FulltextHit& hit : merged) {
        out.row_ids.push_back(hit.row_id);
        out.scores.push_back(hit.score);
    }
    out.nodes_answered = answered;
    out.wire_bytes_moved = wire;
    out.status = IndexStatus::success();
    return out;
}

SignalResponse DistributedFulltextIndex::on_request(const Signal& request) {
    if (request.method == kQueryMethod) {
        std::string decode_error;
        const std::optional<FulltextQueryRequest> message =
            decode_fulltext_query(request.payload, &decode_error);
        if (!message) {
            return SignalResponse{.ok = false,
                                  .error = "fulltext.query: " + decode_error};
        }
        IndexStatus status;
        std::vector<FulltextHit> hits =
            query_local(message->spec(), message->query(), &status);
        if (!status.ok()) {
            return SignalResponse{.ok = false,
                                  .error = "fulltext.query local: " + status.message};
        }
        FulltextQueryResponse response;
        response.from = cluster_.node_id();
        response.hits = std::move(hits);
        return SignalResponse{.ok = true, .payload = encode_fulltext_hits(response)};
    }
    if (delegate_) return delegate_->on_request(request);
    return SignalHandler::on_request(request);  // safe default
}

void DistributedFulltextIndex::on_event(const Signal& event) {
    if (delegate_) delegate_->on_event(event);
}

void DistributedFulltextIndex::on_task(const Signal& task) {
    if (delegate_) delegate_->on_task(task);
}

}  // namespace zetta
