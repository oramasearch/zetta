#include "zetta/index/distributed_fulltext.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <istream>
#include <sstream>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/index/oramacore_fulltext_index.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/data/table.hpp"

namespace zetta::index {
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

// Process-wide validators, compiled once from the schemas (the expensive
// step). The schemas themselves are the public fulltext_query_schema() /
// fulltext_hits_schema() accessors below — the code-owned source of truth
// (#646).
nlohmann::json_schema::json_validator& query_validator() {
    static nlohmann::json_schema::json_validator validator(
        fulltext_query_schema());
    return validator;
}

nlohmann::json_schema::json_validator& hits_validator() {
    static nlohmann::json_schema::json_validator validator(
        fulltext_hits_schema());
    return validator;
}

nlohmann::json_schema::json_validator& stats_validator() {
    static nlohmann::json_schema::json_validator validator(
        fulltext_stats_schema());
    return validator;
}

// The corpus-statistics object both the fulltext.query request (the merged view
// every shard scores with) and the fulltext.stats response (one node's part)
// carry (#851): the document count, each analyzed query term's document
// frequency, and each field's length totals. Every count is a non-negative
// integer; a closed object at every level.
nlohmann::json statistics_schema() {
    return nlohmann::json::parse(R"JSONSCHEMA(
{
  "type": "object",
  "properties": {
    "doc_count": { "type": "integer", "minimum": 0 },
    "term_doc_freq": {
      "type": "object",
      "additionalProperties": { "type": "integer", "minimum": 0 }
    },
    "fields": {
      "type": "object",
      "additionalProperties": {
        "type": "object",
        "properties": {
          "total_len": { "type": "integer", "minimum": 0 },
          "doc_count": { "type": "integer", "minimum": 0 }
        },
        "required": ["total_len", "doc_count"],
        "additionalProperties": false
      }
    }
  },
  "required": ["doc_count", "term_doc_freq", "fields"],
  "additionalProperties": false
}
)JSONSCHEMA");
}

nlohmann::json encode_statistics(const CorpusStatistics& statistics) {
    nlohmann::json terms = nlohmann::json::object();
    for (const auto& [term, frequency] : statistics.term_doc_freq) {
        terms[term] = frequency;
    }
    nlohmann::json fields = nlohmann::json::object();
    for (const auto& [field, lengths] : statistics.fields) {
        fields[field] = nlohmann::json{{"total_len", lengths.total_len},
                                       {"doc_count", lengths.doc_count}};
    }
    return nlohmann::json{{"doc_count", statistics.doc_count},
                          {"term_doc_freq", std::move(terms)},
                          {"fields", std::move(fields)}};
}

// Decode an already schema-validated statistics object.
CorpusStatistics decode_statistics(const nlohmann::json& document) {
    CorpusStatistics statistics;
    statistics.doc_count = document.at("doc_count").get<std::uint64_t>();
    for (const auto& [term, frequency] : document.at("term_doc_freq").items()) {
        statistics.term_doc_freq.emplace(term, frequency.get<std::uint64_t>());
    }
    for (const auto& [field, lengths] : document.at("fields").items()) {
        statistics.fields.emplace(
            field, FieldLengthStatistics{lengths.at("total_len").get<std::uint64_t>(),
                                         lengths.at("doc_count").get<std::uint64_t>()});
    }
    return statistics;
}

// --- partition-body reading (compute-near-data) ------------------------------

// Read the body of a large object by id from whichever of the node's databases
// holds it (its `blob_ref` is a globally-unique `lo:` id). nullopt when no database
// holds the body locally. Mirrors zetta::ddf's local-partition body read.
std::optional<std::string> read_blob_body(Node& node, const std::string& blob_ref) {
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database = node.find_database(name);
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
        const std::shared_ptr<zetta::data::Database> database = node.find_database(name);
        if (!database) continue;
        if (const std::shared_ptr<zetta::data::Table> found = database->find_table(table)) {
            return found->id();
        }
    }
    return table;
}

// Parse a partition body into IndexDocuments through the shared partition-body
// reader, which sniffs Parquet vs the legacy JSON Lines from the bytes. **Fails
// fast** on a body it cannot read (a silent drop would diverge from the
// single-index oracle): returns false + a reason.
bool parse_body_documents(const std::string& body, const std::string& partition_id,
                          std::vector<IndexDocument>& documents,
                          std::uint64_t& rows, std::string* error) {
    zetta::data::PartitionBodyReadResult read =
        zetta::data::read_partition_body(body, partition_id);
    if (!read.ok()) {
        set_error(error, read.error);
        return false;
    }
    for (zetta::data::PartitionRow& row : read.rows) {
        documents.push_back(IndexDocument{std::move(row.id), std::move(row.doc)});
        ++rows;
    }
    return true;
}

}  // namespace

// --- the schema accessors (schema-as-code, #646) ------------------------------

// The fulltext.query REQUEST schema (draft-07) — the code-owned source of
// truth enforced at the decode_fulltext_query boundary (the signal-envelope /
// collective-message pattern); schemas/fulltext_query.schema.json is generated
// from it and drift-checked by tests/test_schemas.cpp. A closed object: the
// index it names (table/kind/fields) + the query (term) are required; backend /
// field / tag / top_k / return_fields / statistics are optional. The same
// request asks a peer for its corpus statistics over fulltext.stats.
const nlohmann::json& fulltext_query_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.FulltextQueryRequest",
  "type": "object",
  "properties": {
    "tag": { "type": "string" },
    "table": { "type": "string", "minLength": 1 },
    "kind": { "type": "string", "enum": ["search", "vector", "feature"] },
    "backend": { "type": "string" },
    "fields": { "type": "array", "minItems": 1, "items": { "type": "string" } },
    "field": { "type": "string" },
    "term": { "type": ["string", "number", "boolean", "null"] },
    "top_k": { "type": "integer", "minimum": 0 },
    "return_fields": { "type": "array", "items": { "type": "string" } }
  },
  "required": ["table", "kind", "fields", "term"],
  "additionalProperties": false
}
)JSONSCHEMA");
        document["properties"]["statistics"] = statistics_schema();
        return document;
    }();
    return schema;
}

// The fulltext.query RESPONSE schema (draft-07) — the code-owned source of
// truth enforced at the decode_fulltext_hits boundary;
// schemas/fulltext_hits.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp. One node's ranked hits: a list of { id, score }
// objects.
const nlohmann::json& fulltext_hits_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
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
          "id": { "type": "string" },
          "score": { "type": "number" },
          "fields": { "type": "object" }
        },
        "required": ["id", "score"],
        "additionalProperties": false
      }
    },
    "sub_indexes": { "type": "integer", "minimum": 0 }
  },
  "required": ["hits"],
  "additionalProperties": false
}
)JSONSCHEMA");
    return schema;
}

// The fulltext.stats RESPONSE schema (draft-07) — the code-owned source of
// truth enforced at the decode_fulltext_stats boundary;
// schemas/fulltext_stats.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp. One node's corpus statistics for a query (#851).
const nlohmann::json& fulltext_stats_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.FulltextStatsResponse",
  "type": "object",
  "properties": {
    "from": { "type": "string" },
    "sub_indexes": { "type": "integer", "minimum": 0 }
  },
  "required": ["statistics"],
  "additionalProperties": false
}
)JSONSCHEMA");
        document["properties"]["statistics"] = statistics_schema();
        return document;
    }();
    return schema;
}

// --- the pure merge ----------------------------------------------------------

std::vector<FulltextHit> merge_fulltext_hits(
    std::span<const std::vector<FulltextHit>> sources, std::size_t top_k) {
    // Dedup by row id, keeping the highest score (a row id should appear in exactly
    // one partition, but a duplicate is collapsed rather than listed twice). std::map
    // gives ascending row-id order, the stable tiebreak.
    std::map<std::string, const FulltextHit*> best;
    for (const std::vector<FulltextHit>& source : sources) {
        for (const FulltextHit& hit : source) {
            const auto [it, inserted] = best.try_emplace(hit.row_id, &hit);
            if (!inserted && hit.score > it->second->score) it->second = &hit;
        }
    }

    std::vector<FulltextHit> merged;
    merged.reserve(best.size());
    for (const auto& [row_id, hit] : best) {
        merged.push_back(*hit);  // carries the owner-attached fields along
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
    q.statistics = statistics;
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
    if (request.return_fields) document["return_fields"] = *request.return_fields;
    if (request.statistics) document["statistics"] = encode_statistics(*request.statistics);
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
    if (const auto fields = document.find("return_fields"); fields != document.end()) {
        request.return_fields = fields->get<std::vector<std::string>>();
    }
    if (const auto statistics = document.find("statistics");
        statistics != document.end()) {
        request.statistics = decode_statistics(*statistics);
    }
    return request;
}

std::string encode_fulltext_hits(const FulltextQueryResponse& response) {
    nlohmann::json document;
    if (!response.from.empty()) document["from"] = response.from;
    nlohmann::json hits = nlohmann::json::array();
    for (const FulltextHit& hit : response.hits) {
        nlohmann::json item{{"id", hit.row_id}, {"score", hit.score}};
        if (hit.fields.is_object()) item["fields"] = hit.fields;
        hits.push_back(std::move(item));
    }
    document["hits"] = std::move(hits);
    document["sub_indexes"] = response.sub_indexes;
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
    response.sub_indexes =
        static_cast<std::size_t>(document.value("sub_indexes", std::uint64_t{0}));
    for (const nlohmann::json& hit : document.at("hits")) {
        response.hits.push_back(FulltextHit{
            hit.at("id").get<std::string>(), hit.at("score").get<double>(),
            hit.value("fields", nlohmann::json{})});
    }
    return response;
}

std::string encode_fulltext_stats(const FulltextStatsResponse& response) {
    nlohmann::json document;
    if (!response.from.empty()) document["from"] = response.from;
    document["statistics"] = encode_statistics(response.statistics);
    document["sub_indexes"] = response.sub_indexes;
    return document.dump();
}

std::optional<FulltextStatsResponse> decode_fulltext_stats(std::string_view payload,
                                                           std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "fulltext stats is not valid JSON");
        return std::nullopt;
    }

    collecting_error_handler handler;
    stats_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    FulltextStatsResponse response;
    response.from = document.value("from", std::string{});
    response.sub_indexes =
        static_cast<std::size_t>(document.value("sub_indexes", std::uint64_t{0}));
    response.statistics = decode_statistics(document.at("statistics"));
    return response;
}

// --- DistributedFulltextIndex ------------------------------------------------

DistributedFulltextIndex::DistributedFulltextIndex(
    zetta::cluster::Cluster& cluster, std::shared_ptr<IndexBackend> backend, SignalHandler* delegate)
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
    for (const zetta::cluster::ClusterPeer& peer : cluster_.peers()) ids.push_back(peer.node_id);
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
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>> owned;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) continue;
        const std::vector<std::string>& owners = partition->owner_nodes();
        if (std::find(owners.begin(), owners.end(), self) == owners.end()) continue;
        owned.push_back(partition);
    }
    std::sort(owned.begin(), owned.end(),
              [](const std::shared_ptr<zetta::data::DatasetPartition>& a,
                 const std::shared_ptr<zetta::data::DatasetPartition>& b) {
                  return a->row_range().start < b->row_range().start;
              });

    const std::lock_guard<std::mutex> lock(mutex_);

    // Drop any sub-indexes previously built for this spec (rebuild semantics).
    const auto prior = sub_specs_.find(spec.key());
    if (prior != sub_specs_.end()) {
        for (const IndexSpec& sub : prior->second) backend_->drop(sub);
        sub_specs_.erase(prior);
    }
    rows_.erase(spec.key());
    std::map<std::string, nlohmann::json> rows;

    std::vector<IndexSpec> subs;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition : owned) {
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

        // Keep each row's indexed values, so a hit can carry them (#1243).
        for (const IndexDocument& document : built_partition.documents) {
            nlohmann::json values = nlohmann::json::object();
            if (document.doc.is_object()) {
                for (const std::string& field : spec.fields) {
                    if (const auto found = document.doc.find(field);
                        found != document.doc.end()) {
                        values[field] = *found;
                    }
                }
            }
            rows.insert_or_assign(document.id, std::move(values));
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
    rows_[spec.key()] = std::move(rows);
    out.status = IndexStatus::success();
    return out;
}

std::vector<FulltextHit> DistributedFulltextIndex::query_local(
    const IndexSpec& spec, const IndexQuery& query, IndexStatus* status,
    std::size_t* sub_indexes, const std::vector<std::string>* return_fields) const {
    const std::lock_guard<std::mutex> lock(mutex_);

    if (sub_indexes) *sub_indexes = 0;
    const auto found = sub_specs_.find(spec.key());
    if (found == sub_specs_.end() || found->second.empty()) {
        // This node holds no partitions of the index — it contributes nothing (Ok).
        if (status) *status = IndexStatus::success();
        return {};
    }
    if (sub_indexes) *sub_indexes = found->second.size();

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
    std::vector<FulltextHit> merged = merge_fulltext_hits(per_partition, query.top_k);
    if (return_fields != nullptr) {
        // This node owns every row it just matched, so it attaches their values
        // here: only the capped hits, never the whole partition (#1243).
        const auto rows = rows_.find(spec.key());
        for (FulltextHit& hit : merged) {
            hit.fields = nlohmann::json::object();
            if (rows == rows_.end()) continue;
            const auto row = rows->second.find(hit.row_id);
            if (row == rows->second.end()) continue;
            for (const std::string& field : *return_fields) {
                if (const auto value = row->second.find(field);
                    value != row->second.end()) {
                    hit.fields[field] = *value;
                }
            }
        }
    }
    return merged;
}

CorpusStatistics DistributedFulltextIndex::collect_local_statistics(
    const IndexSpec& spec, const IndexQuery& query, IndexStatus* status,
    std::size_t* sub_indexes) const {
    const std::lock_guard<std::mutex> lock(mutex_);

    CorpusStatistics statistics;
    if (sub_indexes) *sub_indexes = 0;
    if (status) *status = IndexStatus::success();
    const auto found = sub_specs_.find(spec.key());
    if (found == sub_specs_.end()) return statistics;  // holds none: adds nothing
    if (sub_indexes) *sub_indexes = found->second.size();
    for (const IndexSpec& sub : found->second) {
        const IndexStatus collected = backend_->collect_statistics(sub, query, statistics);
        if (!collected.ok()) {
            if (status) *status = collected;
            return {};
        }
    }
    return statistics;
}

DistributedFulltextIndex::QueryResult DistributedFulltextIndex::query(
    const IndexSpec& spec, const IndexQuery& query,
    std::chrono::milliseconds timeout,
    zetta::observability::SpanRecorder* recorder,
    const zetta::observability::TraceContext* parent,
    const std::vector<std::string>* return_fields) const {
    QueryResult out;

    // Tracing needs both halves; with either missing, spans become no-ops.
    if (!parent || !parent->valid()) recorder = nullptr;
    const zetta::observability::TraceContext trace =
        parent ? *parent : zetta::observability::TraceContext{};

    // Both rounds go to the same peers: a peer that joined between them would be
    // asked for hits scored with statistics that never counted its rows.
    const std::vector<zetta::cluster::ClusterPeer> peers = cluster_.peers();

    FulltextQueryRequest request;
    request.table = spec.table;
    request.kind = spec.kind;
    request.backend = spec.backend;
    request.fields = spec.fields;
    request.field = query.field;
    request.term = query.term;
    request.top_k = query.top_k;
    if (return_fields != nullptr) request.return_fields = *return_fields;

    std::uint64_t wire = 0;
    // Per-peer bytes and time of the statistics round, folded into each peer's
    // contribution below.
    std::map<std::string, std::uint64_t> stats_bytes;
    std::map<std::string, double> stats_latency_ms;

    // (0) The global-statistics round (#851): every sub-index on every node must
    // score with the same corpus statistics, or a per-shard top_k cutoff can drop
    // a row that belongs in the global top k. Sum this node's, gather every
    // peer's, and score everything below with the merge.
    IndexQuery scored = query;
    if (backend_->uses_corpus_statistics() && !scored.statistics) {
        zetta::observability::ScopedSpan stats_span(recorder, trace, "search:statistics");
        IndexStatus stats_status;
        CorpusStatistics merged = collect_local_statistics(spec, query, &stats_status);
        if (!stats_status.ok()) {
            stats_span.finish(/*ok=*/false, stats_status.message);
            out.status = stats_status;
            return out;
        }
        const std::string stats_payload = encode_fulltext_query(request);
        for (const zetta::cluster::ClusterPeer& peer : peers) {
            const auto started = std::chrono::steady_clock::now();
            const zetta::cluster::SignalResponse response = cluster_.request(
                peer.node_id, std::string(kStatsMethod), stats_payload, timeout);
            stats_latency_ms[peer.node_id] =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            if (!response.ok) {
                out.status = IndexStatus::backend_error(
                    "fulltext.stats to peer '" + peer.node_id + "' failed: " +
                    response.error);
                stats_span.finish(/*ok=*/false, out.status.message);
                return out;
            }
            std::string decode_error;
            const std::optional<FulltextStatsResponse> peer_stats =
                decode_fulltext_stats(response.payload, &decode_error);
            if (!peer_stats) {
                out.status = IndexStatus::backend_error(
                    "fulltext stats from peer '" + peer.node_id +
                    "' are malformed: " + decode_error);
                stats_span.finish(/*ok=*/false, out.status.message);
                return out;
            }
            stats_bytes[peer.node_id] = stats_payload.size() + response.payload.size();
            wire += stats_bytes[peer.node_id];
            merged.merge(peer_stats->statistics);
        }
        stats_span.set_attribute("doc_count", merged.doc_count);
        stats_span.finish(/*ok=*/true);
        scored.statistics = std::move(merged);
    }
    request.statistics = scored.statistics;

    // (1) This node's local contribution (across its own partitions).
    zetta::observability::ScopedSpan local_span(recorder, trace, "search:local");
    local_span.set_attribute("node_id", cluster_.node_id());
    IndexStatus local_status;
    std::size_t local_sub_indexes = 0;
    std::vector<FulltextHit> local = query_local(
        spec, scored, &local_status, &local_sub_indexes, return_fields);
    if (!local_status.ok()) {
        local_span.finish(/*ok=*/false, local_status.message);
        out.status = local_status;
        return out;
    }
    local_span.set_attribute("hits", local.size());
    local_span.set_attribute("sub_indexes", local_sub_indexes);
    NodeContribution local_contribution;
    local_contribution.node_id = cluster_.node_id();
    local_contribution.local = true;
    local_contribution.hits = local.size();
    local_contribution.sub_indexes = local_sub_indexes;
    local_contribution.latency_ms = local_span.elapsed_ms();
    local_span.finish(/*ok=*/true);
    out.contributions.push_back(std::move(local_contribution));

    std::vector<std::vector<FulltextHit>> sources;
    sources.push_back(std::move(local));
    std::size_t answered = 1;

    // (2) Scatter the query (with the merged statistics) to every peer and gather
    // their ranked hits.
    const std::string payload = encode_fulltext_query(request);

    for (const zetta::cluster::ClusterPeer& peer : peers) {
        zetta::observability::ScopedSpan peer_span(recorder, trace, "search:peer");
        peer_span.set_attribute("node_id", peer.node_id);
        const zetta::cluster::SignalResponse response = cluster_.request(
            peer.node_id, std::string(kQueryMethod), payload, timeout);
        wire += payload.size();
        if (!response.ok) {
            out.status = IndexStatus::backend_error(
                "fulltext.query to peer '" + peer.node_id + "' failed: " +
                response.error);
            peer_span.finish(/*ok=*/false, out.status.message);
            out.contributions.clear();
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
            peer_span.finish(/*ok=*/false, out.status.message);
            out.contributions.clear();
            return out;
        }
        NodeContribution peer_contribution;
        peer_contribution.node_id = peer.node_id;
        peer_contribution.hits = hits->hits.size();
        peer_contribution.sub_indexes = hits->sub_indexes;
        peer_contribution.latency_ms =
            peer_span.elapsed_ms() + stats_latency_ms[peer.node_id];
        peer_contribution.wire_bytes =
            payload.size() + response.payload.size() + stats_bytes[peer.node_id];
        peer_span.set_attribute("hits", hits->hits.size());
        peer_span.set_attribute("sub_indexes", hits->sub_indexes);
        peer_span.set_attribute("wire_bytes", peer_contribution.wire_bytes);
        peer_span.finish(/*ok=*/true);
        out.contributions.push_back(std::move(peer_contribution));
        sources.push_back(std::move(hits->hits));
        ++answered;
    }

    // (3) Merge every contribution into the global ranking.
    const std::vector<FulltextHit> merged =
        merge_fulltext_hits(sources, scored.top_k);
    out.row_ids.reserve(merged.size());
    out.scores.reserve(merged.size());
    out.fields.reserve(merged.size());
    for (const FulltextHit& hit : merged) {
        out.row_ids.push_back(hit.row_id);
        out.scores.push_back(hit.score);
        out.fields.push_back(hit.fields);
    }
    out.nodes_answered = answered;
    out.wire_bytes_moved = wire;
    out.status = IndexStatus::success();
    return out;
}

zetta::cluster::SignalResponse DistributedFulltextIndex::on_request(const zetta::cluster::Signal& request) {
    if (request.method == kQueryMethod) {
        std::string decode_error;
        const std::optional<FulltextQueryRequest> message =
            decode_fulltext_query(request.payload, &decode_error);
        if (!message) {
            return zetta::cluster::SignalResponse{.ok = false,
                                  .error = "fulltext.query: " + decode_error};
        }
        IndexStatus status;
        std::size_t sub_indexes = 0;
        std::vector<FulltextHit> hits = query_local(
            message->spec(), message->query(), &status, &sub_indexes,
            message->return_fields ? &*message->return_fields : nullptr);
        if (!status.ok()) {
            return zetta::cluster::SignalResponse{.ok = false,
                                  .error = "fulltext.query local: " + status.message};
        }
        FulltextQueryResponse response;
        response.from = cluster_.node_id();
        response.hits = std::move(hits);
        response.sub_indexes = sub_indexes;
        return zetta::cluster::SignalResponse{.ok = true, .payload = encode_fulltext_hits(response)};
    }
    if (request.method == kStatsMethod) {
        std::string decode_error;
        const std::optional<FulltextQueryRequest> message =
            decode_fulltext_query(request.payload, &decode_error);
        if (!message) {
            return zetta::cluster::SignalResponse{.ok = false,
                                  .error = "fulltext.stats: " + decode_error};
        }
        IndexStatus status;
        std::size_t sub_indexes = 0;
        FulltextStatsResponse response;
        response.statistics = collect_local_statistics(
            message->spec(), message->query(), &status, &sub_indexes);
        if (!status.ok()) {
            return zetta::cluster::SignalResponse{.ok = false,
                                  .error = "fulltext.stats local: " + status.message};
        }
        response.from = cluster_.node_id();
        response.sub_indexes = sub_indexes;
        return zetta::cluster::SignalResponse{.ok = true,
                                              .payload = encode_fulltext_stats(response)};
    }
    if (delegate_) return delegate_->on_request(request);
    return SignalHandler::on_request(request);  // safe default
}

void DistributedFulltextIndex::on_event(const zetta::cluster::Signal& event) {
    if (delegate_) delegate_->on_event(event);
}

void DistributedFulltextIndex::on_task(const zetta::cluster::Signal& task) {
    if (delegate_) delegate_->on_task(task);
}

}  // namespace zetta::index