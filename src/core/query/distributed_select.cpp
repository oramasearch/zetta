#include "zetta/query/distributed_select.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_manifest.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/query/predicate.hpp"

namespace zetta::query {
namespace {

// --- schema boundary (the collective-message / fulltext pattern) --------------

// Collects the first validation failure without letting it cross the validator
// boundary as an exception.
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

nlohmann::json_schema::json_validator& request_validator() {
    static nlohmann::json_schema::json_validator validator(
        partition_scan_request_schema());
    return validator;
}

nlohmann::json_schema::json_validator& response_validator() {
    static nlohmann::json_schema::json_validator validator(
        partition_scan_response_schema());
    return validator;
}

// --- the AST enums as wire names ---------------------------------------------

std::string_view compare_op_name(CompareOp op) {
    switch (op) {
        case CompareOp::Equal:        return "eq";
        case CompareOp::NotEqual:     return "ne";
        case CompareOp::Less:         return "lt";
        case CompareOp::LessEqual:    return "le";
        case CompareOp::Greater:      return "gt";
        case CompareOp::GreaterEqual: return "ge";
    }
    return "eq";
}

std::optional<CompareOp> compare_op_from_name(std::string_view name) {
    if (name == "eq") return CompareOp::Equal;
    if (name == "ne") return CompareOp::NotEqual;
    if (name == "lt") return CompareOp::Less;
    if (name == "le") return CompareOp::LessEqual;
    if (name == "gt") return CompareOp::Greater;
    if (name == "ge") return CompareOp::GreaterEqual;
    return std::nullopt;
}

std::string_view aggregate_func_name(AggregateFunc func) {
    switch (func) {
        case AggregateFunc::Count: return "count";
        case AggregateFunc::Sum:   return "sum";
        case AggregateFunc::Min:   return "min";
        case AggregateFunc::Max:   return "max";
        case AggregateFunc::Avg:   return "avg";
    }
    return "count";
}

std::optional<AggregateFunc> aggregate_func_from_name(std::string_view name) {
    if (name == "count") return AggregateFunc::Count;
    if (name == "sum")   return AggregateFunc::Sum;
    if (name == "min")   return AggregateFunc::Min;
    if (name == "max")   return AggregateFunc::Max;
    if (name == "avg")   return AggregateFunc::Avg;
    return std::nullopt;
}

// --- group keys ---------------------------------------------------------------

// A canonical string for a group key, so the merge treats 1 and 1.0 as one group
// (nlohmann compares them equal, but their dumps differ).
std::string group_key_of(const nlohmann::json& key) {
    if (key.is_number()) return "n:" + nlohmann::json(key.get<double>()).dump();
    return "j:" + key.dump();
}

// Deterministic ordering for result rows: nulls, then booleans, then numbers,
// then strings, then anything else by its dump.
int key_rank(const nlohmann::json& key) {
    if (key.is_null()) return 0;
    if (key.is_boolean()) return 1;
    if (key.is_number()) return 2;
    if (key.is_string()) return 3;
    return 4;
}

bool key_less(const nlohmann::json& left, const nlohmann::json& right) {
    const int left_rank = key_rank(left);
    const int right_rank = key_rank(right);
    if (left_rank != right_rank) return left_rank < right_rank;
    switch (left_rank) {
        case 1: return static_cast<int>(left.get<bool>()) <
                       static_cast<int>(right.get<bool>());
        case 2: return left.get<double>() < right.get<double>();
        case 3: return left.get_ref<const std::string&>() <
                       right.get_ref<const std::string&>();
        default: return left.dump() < right.dump();
    }
}

// --- aggregate accumulation ---------------------------------------------------

// A JSON integer stays an integer through sum/min/max, so `sum(qty)` reads back
// as an integer rather than a float.
bool is_json_integer(const nlohmann::json& cell) {
    return cell.is_number_integer() || cell.is_number_unsigned();
}

AggregatePartial empty_partial(AggregateFunc func) {
    AggregatePartial partial;
    switch (func) {
        case AggregateFunc::Min:
            partial.value = std::numeric_limits<double>::infinity();
            break;
        case AggregateFunc::Max:
            partial.value = -std::numeric_limits<double>::infinity();
            break;
        default:
            partial.value = 0.0;
            break;
    }
    return partial;
}

// Fold one row into one aggregate's partial.
void accumulate(AggregatePartial& partial, AggregateFunc func,
                const std::string& column, const nlohmann::json& row) {
    if (func == AggregateFunc::Count && column.empty()) {
        partial.value += 1.0;  // COUNT(*) — every row in the group
        ++partial.count;
        return;
    }
    const auto cell = row.find(column);
    if (cell == row.end() || cell->is_null()) return;
    if (func == AggregateFunc::Count) {
        partial.value += 1.0;  // COUNT(column) — the non-null values
        ++partial.count;
        return;
    }
    if (!cell->is_number()) return;  // a non-numeric cell contributes nothing
    const double value = cell->get<double>();
    switch (func) {
        case AggregateFunc::Sum:
        case AggregateFunc::Avg:
            partial.value += value;
            break;
        case AggregateFunc::Min:
            partial.value = std::min(partial.value, value);
            break;
        case AggregateFunc::Max:
            partial.value = std::max(partial.value, value);
            break;
        default:
            break;
    }
    if (!is_json_integer(*cell)) partial.integral = false;
    ++partial.count;
}

// Fold one responder's partial into the merged one. This is where the design's
// "aggregates merge, not concatenate" lives: count/sum add, min/max reduce, and
// avg keeps the summed value beside the summed count so the division happens
// once, over the whole dataset.
void merge_partial(AggregatePartial& into, AggregateFunc func,
                   const AggregatePartial& from) {
    if (from.count == 0) return;  // contributed nothing; leave the identity alone
    switch (func) {
        case AggregateFunc::Count:
        case AggregateFunc::Sum:
        case AggregateFunc::Avg:
            into.value += from.value;
            break;
        case AggregateFunc::Min:
            into.value = std::min(into.value, from.value);
            break;
        case AggregateFunc::Max:
            into.value = std::max(into.value, from.value);
            break;
    }
    into.count += from.count;
    if (!from.integral) into.integral = false;
}

// A whole-number value of an integral aggregate comes back as a JSON integer;
// everything else (means, floats) as a double — matching the local grouped path.
nlohmann::json number_json(double value, bool integral) {
    if (integral && std::isfinite(value) && std::floor(value) == value &&
        value >= -9.2e18 && value <= 9.2e18) {
        return nlohmann::json(static_cast<std::int64_t>(value));
    }
    return nlohmann::json(value);
}

// The finished cell for one merged aggregate. An aggregate no row contributed to
// is NULL (SQL's answer for SUM/MIN/MAX over nothing); COUNT is always a number.
nlohmann::json finish_aggregate(AggregateFunc func, const AggregatePartial& partial) {
    switch (func) {
        case AggregateFunc::Count:
            return nlohmann::json(static_cast<std::int64_t>(partial.value));
        case AggregateFunc::Avg:
            if (partial.count == 0) return nlohmann::json();
            return nlohmann::json(partial.value /
                                  static_cast<double>(partial.count));
        default:
            if (partial.count == 0) return nlohmann::json();
            return number_json(partial.value, partial.integral);
    }
}

// The result column name of an aggregate, matching the local grouped path's
// defaults (`count`, `sum_x`, `min_x`, `max_x`, `mean_x`) so the same SQL names
// its columns the same way whichever path answers it.
std::string aggregate_output_name(const AggregateExpr& aggregate,
                                  const std::string& alias) {
    if (!alias.empty()) return alias;
    if (aggregate.func == AggregateFunc::Count) return "count";
    const std::string_view op =
        aggregate.func == AggregateFunc::Avg ? "mean" : aggregate_func_name(aggregate.func);
    return std::string(op) + "_" + aggregate.column;
}

// The result columns of a grouped SELECT, in select order (a plain SELECT's
// columns are the executor's own projection).
std::vector<std::string> grouped_columns(const SelectStmt& statement) {
    std::vector<std::string> columns;
    for (const GroupedItem& item : statement.items) {
        columns.push_back(item.aggregate
                              ? aggregate_output_name(*item.aggregate, item.alias)
                              : (item.alias.empty() ? item.column : item.alias));
    }
    return columns;
}

// --- body reading -------------------------------------------------------------

// Read a partition body from its path. Off the Node tree entirely: a body is
// content-addressed and written once, so its bytes never change under a reader.
std::optional<std::string> read_body_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) return std::nullopt;
    return buffer.str();
}

}  // namespace

// --- schemas ------------------------------------------------------------------

const nlohmann::json& partition_scan_request_schema() {
    static const nlohmann::json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"$id", "https://zetta.orama.com/schemas/partition_scan_request.schema.json"},
        {"title", "PartitionScanRequest"},
        {"description",
         "A scatter-gather SELECT's ask of one partition owner: scan these "
         "partitions of this table, filter them, and return rows or per-group "
         "aggregate partials (#833)."},
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"table", "partitions"}},
        {"properties",
         {{"table", {{"type", "string"}, {"minLength", 1}}},
          {"partitions",
           {{"type", "array"}, {"items", {{"type", "string"}, {"minLength", 1}}}}},
          {"where",
           {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"column", "op", "value"}},
            {"properties",
             {{"column", {{"type", "string"}, {"minLength", 1}}},
              {"op", {{"enum", {"eq", "ne", "lt", "le", "gt", "ge"}}}},
              {"value", nlohmann::json::object()}}}}},
          {"columns", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"group_by", {{"type", "string"}}},
          {"aggregates",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"func"}},
              {"properties",
               {{"func", {{"enum", {"count", "sum", "min", "max", "avg"}}}},
                {"column", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

const nlohmann::json& partition_scan_response_schema() {
    static const nlohmann::json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"$id", "https://zetta.orama.com/schemas/partition_scan_response.schema.json"},
        {"title", "PartitionScanResponse"},
        {"description",
         "One partition owner's answer to a scatter-gather SELECT: the surviving "
         "rows per partition, or per-group aggregate PARTIALS carrying the row "
         "counts the merge weights them by (#833)."},
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"from"}},
        {"properties",
         {{"from", {{"type", "string"}}},
          {"partitions_read", {{"type", "integer"}, {"minimum", 0}}},
          {"rows_scanned", {{"type", "integer"}, {"minimum", 0}}},
          {"bytes_read", {{"type", "integer"}, {"minimum", 0}}},
          {"blocks",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"partition", "rows"}},
              {"properties",
               {{"partition", {{"type", "string"}, {"minLength", 1}}},
                {"rows", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}}}}},
          {"groups",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"key", "aggregates"}},
              {"properties",
               {{"key", nlohmann::json::object()},
                {"aggregates",
                 {{"type", "array"},
                  {"items",
                   {{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"value", "count"}},
                    {"properties",
                     {{"value", {{"type", "number"}}},
                      {"count", {{"type", "integer"}, {"minimum", 0}}},
                      {"integral", {{"type", "boolean"}}}}}}}}}}}}}}},
          {"columns_seen",
           {{"type", "array"},
            {"description",
             "every column any scanned row carried, before the WHERE (#1218)"},
            {"items", {{"type", "string"}}}}}}}};
    return schema;
}

// --- wire messages ------------------------------------------------------------

std::string encode_partition_scan_request(const PartitionScanRequest& request) {
    nlohmann::json document;
    document["table"] = request.table;
    document["partitions"] = request.partitions;
    if (request.where) {
        document["where"] = {{"column", request.where->column},
                             {"op", std::string(compare_op_name(request.where->op))},
                             {"value", request.where->value}};
    }
    if (!request.columns.empty()) document["columns"] = request.columns;
    if (request.aggregating()) {
        if (!request.group_by.empty()) document["group_by"] = request.group_by;
        nlohmann::json aggregates = nlohmann::json::array();
        for (const ScanAggregate& aggregate : request.aggregates) {
            nlohmann::json entry{{"func", std::string(aggregate_func_name(aggregate.func))}};
            if (!aggregate.column.empty()) entry["column"] = aggregate.column;
            aggregates.push_back(std::move(entry));
        }
        document["aggregates"] = std::move(aggregates);
    }
    return document.dump();
}

std::optional<PartitionScanRequest> decode_partition_scan_request(
    std::string_view payload, std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "partition scan request is not valid JSON");
        return std::nullopt;
    }
    collecting_error_handler handler;
    request_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    PartitionScanRequest request;
    request.table = document.at("table").get<std::string>();
    request.partitions = document.at("partitions").get<std::vector<std::string>>();
    if (document.contains("where")) {
        const nlohmann::json& where = document.at("where");
        const std::optional<CompareOp> op =
            compare_op_from_name(where.at("op").get<std::string>());
        if (!op) {  // the schema enum guards this; stay defensive
            set_error(error, "/where/op: unknown comparison");
            return std::nullopt;
        }
        WhereClause clause;
        clause.column = where.at("column").get<std::string>();
        clause.op = *op;
        clause.value = where.at("value");
        request.where = std::move(clause);
    }
    if (document.contains("columns")) {
        request.columns = document.at("columns").get<std::vector<std::string>>();
    }
    request.group_by = document.value("group_by", std::string{});
    if (document.contains("aggregates")) {
        for (const nlohmann::json& entry : document.at("aggregates")) {
            const std::optional<AggregateFunc> func =
                aggregate_func_from_name(entry.at("func").get<std::string>());
            if (!func) {
                set_error(error, "/aggregates: unknown aggregate function");
                return std::nullopt;
            }
            request.aggregates.push_back(
                ScanAggregate{*func, entry.value("column", std::string{})});
        }
    }
    return request;
}

std::string encode_partition_scan_response(const PartitionScanResponse& response) {
    nlohmann::json document;
    document["from"] = response.from;
    document["partitions_read"] = response.partitions_read;
    document["rows_scanned"] = response.rows_scanned;
    document["bytes_read"] = response.bytes_read;
    if (!response.blocks.empty()) {
        nlohmann::json blocks = nlohmann::json::array();
        for (const ScanBlock& block : response.blocks) {
            blocks.push_back({{"partition", block.partition}, {"rows", block.rows}});
        }
        document["blocks"] = std::move(blocks);
    }
    if (!response.groups.empty()) {
        nlohmann::json groups = nlohmann::json::array();
        for (const GroupPartial& group : response.groups) {
            nlohmann::json aggregates = nlohmann::json::array();
            for (const AggregatePartial& partial : group.aggregates) {
                // An unbounded identity (min/max over nothing) is not a JSON
                // number, so an empty partial travels as a zero with count 0 —
                // the merge skips it either way.
                const double value =
                    partial.count == 0 || !std::isfinite(partial.value) ? 0.0
                                                                        : partial.value;
                aggregates.push_back({{"value", value},
                                      {"count", partial.count},
                                      {"integral", partial.integral}});
            }
            groups.push_back({{"key", group.key}, {"aggregates", std::move(aggregates)}});
        }
        document["groups"] = std::move(groups);
    }
    if (response.columns_seen) document["columns_seen"] = *response.columns_seen;
    return document.dump();
}

std::optional<PartitionScanResponse> decode_partition_scan_response(
    std::string_view payload, std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "partition scan response is not valid JSON");
        return std::nullopt;
    }
    collecting_error_handler handler;
    response_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    PartitionScanResponse response;
    response.from = document.at("from").get<std::string>();
    response.partitions_read = document.value("partitions_read", std::uint64_t{0});
    response.rows_scanned = document.value("rows_scanned", std::uint64_t{0});
    response.bytes_read = document.value("bytes_read", std::uint64_t{0});
    if (document.contains("blocks")) {
        for (const nlohmann::json& block : document.at("blocks")) {
            ScanBlock decoded;
            decoded.partition = block.at("partition").get<std::string>();
            for (const nlohmann::json& row : block.at("rows")) decoded.rows.push_back(row);
            response.blocks.push_back(std::move(decoded));
        }
    }
    if (document.contains("groups")) {
        for (const nlohmann::json& group : document.at("groups")) {
            GroupPartial decoded;
            decoded.key = group.at("key");
            for (const nlohmann::json& aggregate : group.at("aggregates")) {
                AggregatePartial partial;
                partial.value = aggregate.at("value").get<double>();
                partial.count = aggregate.at("count").get<std::uint64_t>();
                partial.integral = aggregate.value("integral", true);
                decoded.aggregates.push_back(partial);
            }
            response.groups.push_back(std::move(decoded));
        }
    }
    if (document.contains("columns_seen")) {
        response.columns_seen =
            document.at("columns_seen").get<std::vector<std::string>>();
    }
    return response;
}

// --- the local scan -----------------------------------------------------------

GroupAccumulator::GroupAccumulator(std::string group_by,
                                   std::vector<ScanAggregate> aggregates)
    : group_by_(std::move(group_by)), aggregates_(std::move(aggregates)) {}

GroupAccumulator::GroupAccumulator(const SelectStmt& statement)
    : GroupAccumulator(statement.group_by.empty() ? std::string{}
                                                  : statement.group_by.front(),
                       scan_aggregates_of(statement)) {}

void GroupAccumulator::add(const nlohmann::json& row) {
    nlohmann::json key;  // null: no group key, or a row without the key column
    if (!group_by_.empty()) {
        const auto cell = row.find(group_by_);
        if (cell != row.end()) key = *cell;
    }
    const std::string canonical = group_key_of(key);
    auto slot = index_.find(canonical);
    if (slot == index_.end()) {
        GroupPartial partial;
        partial.key = std::move(key);
        partial.aggregates.reserve(aggregates_.size());
        for (const ScanAggregate& aggregate : aggregates_) {
            partial.aggregates.push_back(empty_partial(aggregate.func));
        }
        slot = index_.emplace(canonical, groups_.size()).first;
        groups_.push_back(std::move(partial));
    }
    GroupPartial& partial = groups_[slot->second];
    for (std::size_t i = 0; i < aggregates_.size(); ++i) {
        accumulate(partial.aggregates[i], aggregates_[i].func, aggregates_[i].column,
                   row);
    }
}

std::vector<GroupPartial> GroupAccumulator::take() {
    index_.clear();
    return std::exchange(groups_, {});
}

std::vector<ScanAggregate> scan_aggregates_of(const SelectStmt& statement) {
    std::vector<ScanAggregate> aggregates;
    for (const GroupedItem& item : statement.items) {
        if (!item.aggregate) continue;
        aggregates.push_back(ScanAggregate{item.aggregate->func, item.aggregate->column});
    }
    return aggregates;
}

std::vector<std::string> aggregate_result_columns(const SelectStmt& statement) {
    return grouped_columns(statement);
}

PartitionScanResponse scan_owned_partitions(const std::vector<OwnedPartition>& owned,
                                            const PartitionScanRequest& request) {
    PartitionScanResponse response;
    const bool aggregating = request.aggregating();
    GroupAccumulator groups(request.group_by, request.aggregates);

    // The columns the scanned rows carry, so the coordinator can tell a column
    // that holds only nulls from one that does not exist.
    std::vector<std::string> columns_seen;
    std::unordered_set<std::string> seen;

    std::unordered_map<std::string, const OwnedPartition*> by_id;
    for (const OwnedPartition& partition : owned) by_id.emplace(partition.id, &partition);

    for (const std::string& wanted : request.partitions) {
        const auto found = by_id.find(wanted);
        if (found == by_id.end()) {
            // The coordinator asked for a partition this node does not own (a
            // stale catalog snapshot). Fail rather than answer short.
            response.error = "partition '" + wanted + "' is not owned here";
            return response;
        }
        const OwnedPartition& partition = *found->second;
        const std::optional<std::string> body = read_body_file(partition.body);
        if (!body) {
            response.error = "partition '" + wanted + "' body is not readable at " +
                             partition.body.string();
            return response;
        }
        response.bytes_read += body->size();
        const zetta::data::PartitionBodyReadResult read =
            zetta::data::read_partition_body(*body, partition.id);
        if (!read.ok()) {
            response.error = read.error;
            return response;
        }
        ++response.partitions_read;

        ScanBlock block;
        block.partition = partition.id;
        for (const zetta::data::PartitionRow& stored : read.rows) {
            ++response.rows_scanned;
            const nlohmann::json& row = stored.doc;
            if (!row.is_object()) continue;
            for (auto field = row.begin(); field != row.end(); ++field) {
                if (seen.insert(field.key()).second) columns_seen.push_back(field.key());
            }
            if (!row_matches(row, request.where)) continue;
            if (aggregating) {
                groups.add(row);
                continue;
            }
            if (request.columns.empty()) {
                block.rows.push_back(row);
            } else {
                nlohmann::json projected = nlohmann::json::object();
                for (const std::string& column : request.columns) {
                    const auto cell = row.find(column);
                    projected[column] = cell == row.end() ? nlohmann::json() : *cell;
                }
                block.rows.push_back(std::move(projected));
            }
        }
        if (!aggregating) response.blocks.push_back(std::move(block));
    }
    if (aggregating) response.groups = groups.take();
    response.columns_seen = std::move(columns_seen);
    return response;
}

// --- DistributedSelect --------------------------------------------------------

namespace {

// A partition of the table as the coordinator sees it: who owns it and where it
// sits in the dataset's row order.
struct PlannedPartition {
    std::string id;
    std::uint64_t row_start = 0;
    std::vector<std::string> owners;
};

// The table behind a `tbl:` id, or an empty handle when this node does not hold
// it. Needed at plan time for the partition manifest.
std::shared_ptr<zetta::data::Table> find_table_by_id(Node& node,
                                                     const std::string& table_id) {
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database = node.find_database(name);
        if (!database) continue;
        if (auto table = database->find_table_by_id(table_id)) return table;
    }
    return nullptr;
}

// **The completeness check** (#837). A partition whose catalog entry has not
// reached this node is *invisible*, not unreachable — the reachable-owner guard
// below never fires for it, so without this the plan is simply short and the
// answer comes back a fraction of the truth looking perfectly healthy.
//
// The dataset's manifest is the authority: it records how many `part:` entries the
// finished write produced. When this node can see fewer, the dataset is degraded
// here and the query FAILS, exactly as an unreachable owner already does.
//
// A table with no manifest is not checked — it was written before manifests
// existed, or by a path that does not produce one — because failing every read of
// such a dataset would be a far worse regression than the hole this closes.
std::string catalog_completeness_error(Node& node, const std::string& table_id,
                                       std::size_t visible) {
    const std::shared_ptr<zetta::data::Table> table = find_table_by_id(node, table_id);
    if (!table || !table->partition_manifest()) return {};
    return zetta::data::catalog_shortfall(*table->partition_manifest(),
                                          static_cast<std::uint64_t>(visible));
}

std::vector<PlannedPartition> plan_partitions(Node& node, const std::string& table_id) {
    std::vector<PlannedPartition> planned;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) continue;
        if (partition->blob_ref().empty()) continue;  // unplaced / empty
        planned.push_back(PlannedPartition{partition->id(),
                                           partition->row_range().start,
                                           partition->owner_nodes()});
    }
    std::sort(planned.begin(), planned.end(),
              [](const PlannedPartition& a, const PlannedPartition& b) {
                  if (a.row_start != b.row_start) return a.row_start < b.row_start;
                  return a.id < b.id;
              });
    return planned;
}

// Turn a SELECT into the ask sent to every responder. The WHERE and the
// projection are pushed to the owner (filter + prune near the data, so only the
// wanted columns of the surviving rows cross the wire); column-level access
// masking stays with the coordinator, which is the node that knows the caller.
PartitionScanRequest scan_request_for(const SelectStmt& statement,
                                      const std::string& table_id) {
    PartitionScanRequest request;
    request.table = table_id;
    request.where = statement.where;
    if (statement.aggregating()) {
        if (!statement.group_by.empty()) request.group_by = statement.group_by.front();
        request.aggregates = scan_aggregates_of(statement);
    } else if (!statement.all) {
        request.columns = statement.columns;
    }
    return request;
}

}  // namespace

DistributedSelect::DistributedSelect(Node& node, zetta::cluster::Cluster& cluster,
                                     zetta::cluster::SignalHandler* delegate)
    : node_(node), cluster_(cluster), delegate_(delegate) {
    cluster_.set_signal_handler(this);
}

DistributedSelect::~DistributedSelect() {
    // Restore the handler the node would otherwise run.
    cluster_.set_signal_handler(delegate_);
}

std::uint64_t DistributedSelect::scans_served() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scans_served_;
}

void DistributedSelect::refresh() {
    // Which tables are partition-only, and which database holds each one's bodies.
    std::unordered_map<std::string, std::shared_ptr<zetta::data::Database>> owners;
    for (const std::string& name : node_.database_names()) {
        const std::shared_ptr<zetta::data::Database> database = node_.find_database(name);
        if (!database) continue;
        for (const std::shared_ptr<zetta::data::Table>& table : database->tables()) {
            if (table && table->partition_only()) owners.emplace(table->id(), database);
        }
    }

    const std::string& self = node_.node_id();
    std::map<std::string, std::vector<OwnedPartition>> owned;
    std::string fingerprint;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node_.dataset_partitions()) {
        if (!partition || partition->blob_ref().empty()) continue;
        const auto database = owners.find(partition->table());
        if (database == owners.end()) continue;
        const std::vector<std::string>& nodes = partition->owner_nodes();
        if (std::find(nodes.begin(), nodes.end(), self) == nodes.end()) continue;
        fingerprint += partition->id();
        fingerprint += '=';
        fingerprint += partition->blob_ref();
        fingerprint += '\n';
        owned[partition->table()].push_back(
            OwnedPartition{partition->id(), partition->row_range().start,
                           database->second->large_objects().body_path(
                               partition->blob_ref())});
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (fingerprint == fingerprint_ && !fingerprint.empty()) return;
    fingerprint_ = std::move(fingerprint);
    owned_ = std::move(owned);
}

zetta::cluster::SignalResponse DistributedSelect::on_request(
    const zetta::cluster::Signal& request) {
    if (request.method == kScanMethod) {
        std::string decode_error;
        const std::optional<PartitionScanRequest> message =
            decode_partition_scan_request(request.payload, &decode_error);
        if (!message) {
            return zetta::cluster::SignalResponse{
                .ok = false, .error = "sql.scan: " + decode_error};
        }
        std::vector<OwnedPartition> owned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = owned_.find(message->table);
            if (found != owned_.end()) owned = found->second;
            ++scans_served_;
        }
        PartitionScanResponse response = scan_owned_partitions(owned, *message);
        if (!response.ok()) {
            return zetta::cluster::SignalResponse{
                .ok = false, .error = "sql.scan local: " + response.error};
        }
        response.from = cluster_.node_id();
        return zetta::cluster::SignalResponse{
            .ok = true, .payload = encode_partition_scan_response(response)};
    }
    if (delegate_) return delegate_->on_request(request);
    return SignalHandler::on_request(request);  // safe default
}

void DistributedSelect::on_event(const zetta::cluster::Signal& event) {
    if (delegate_) delegate_->on_event(event);
}

void DistributedSelect::on_task(const zetta::cluster::Signal& task) {
    if (delegate_) delegate_->on_task(task);
}

// --- the merge ----------------------------------------------------------------

namespace {

// Assign every partition to exactly ONE reachable owner, so a partition with
// replicas is read once rather than once per copy. `reachable` is this node plus
// its connected peers; a partition none of whose owners is reachable fails the
// query — with RF=1 there is no second copy to read, and a short answer to an
// exact query is the outcome that must never ship.
std::string assign_responders(
    const std::vector<PlannedPartition>& planned, const std::string& self,
    const std::unordered_set<std::string>& reachable,
    std::map<std::string, std::vector<std::string>>& by_node) {
    for (const PlannedPartition& partition : planned) {
        const std::string* chosen = nullptr;
        // Prefer this node, so a single-node cluster never opens a connection and
        // a coordinator that owns a partition reads it from its own disk.
        for (const std::string& owner : partition.owners) {
            if (owner == self) { chosen = &owner; break; }
        }
        if (chosen == nullptr) {
            for (const std::string& owner : partition.owners) {
                if (reachable.contains(owner)) { chosen = &owner; break; }
            }
        }
        if (chosen == nullptr) {
            std::string owners;
            for (const std::string& owner : partition.owners) {
                if (!owners.empty()) owners += ", ";
                owners += owner;
            }
            return "partition '" + partition.id + "' has no reachable owner (" +
                   (owners.empty() ? "unplaced" : owners) +
                   ") — the dataset is degraded; re-run the ingest";
        }
        by_node[*chosen].push_back(partition.id);
    }
    return {};
}

}  // namespace

// Rows are emitted in the dataset's row order, independent of which node
// answered; groups are merged per key and finished once.
SelectOutcome merge_scan_responses(const SelectStmt& statement,
                                   const std::vector<std::string>& partition_order,
                                   const std::vector<PartitionScanResponse>& answers) {
    SelectOutcome outcome;
    // The table's columns: the union of what every responder saw, complete only
    // when every responder said.
    std::unordered_set<std::string> seen;
    outcome.table_columns_complete = !answers.empty();
    for (const PartitionScanResponse& answer : answers) {
        if (!answer.columns_seen) {
            outcome.table_columns_complete = false;
            continue;
        }
        for (const std::string& column : *answer.columns_seen) {
            if (seen.insert(column).second) outcome.table_columns.push_back(column);
        }
    }
    if (statement.aggregating()) outcome.columns = grouped_columns(statement);
    if (!statement.aggregating()) {
        std::unordered_map<std::string, const std::vector<nlohmann::json>*> blocks;
        for (const PartitionScanResponse& answer : answers) {
            for (const ScanBlock& block : answer.blocks) {
                blocks.emplace(block.partition, &block.rows);
            }
        }
        for (const std::string& partition : partition_order) {
            const auto found = blocks.find(partition);
            if (found == blocks.end()) continue;
            for (const nlohmann::json& row : *found->second) {
                outcome.rows.push_back(row);
            }
        }
        return outcome;
    }

    // The select list, in order: key passthroughs and aggregates.
    std::vector<AggregateFunc> functions;
    for (const GroupedItem& item : statement.items) {
        if (item.aggregate) functions.push_back(item.aggregate->func);
    }

    std::unordered_map<std::string, std::size_t> index;
    std::vector<GroupPartial> merged;
    for (const PartitionScanResponse& answer : answers) {
        for (const GroupPartial& group : answer.groups) {
            const std::string canonical = group_key_of(group.key);
            auto slot = index.find(canonical);
            if (slot == index.end()) {
                GroupPartial fresh;
                fresh.key = group.key;
                for (const AggregateFunc func : functions) {
                    fresh.aggregates.push_back(empty_partial(func));
                }
                slot = index.emplace(canonical, merged.size()).first;
                merged.push_back(std::move(fresh));
            }
            GroupPartial& into = merged[slot->second];
            for (std::size_t i = 0; i < functions.size() && i < group.aggregates.size();
                 ++i) {
                merge_partial(into.aggregates[i], functions[i], group.aggregates[i]);
            }
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const GroupPartial& a, const GroupPartial& b) {
                  return key_less(a.key, b.key);
              });
    // A whole-table aggregate answers one row even when no row survived:
    // `SELECT count(*) FROM t WHERE …` over nothing is 0, not an empty result
    // (the SUM/MIN/MAX/AVG beside it are NULL), as in Postgres.
    if (statement.group_by.empty() && merged.empty()) {
        GroupPartial empty;
        for (const AggregateFunc func : functions) {
            empty.aggregates.push_back(empty_partial(func));
        }
        merged.push_back(std::move(empty));
    }

    for (const GroupPartial& group : merged) {
        nlohmann::json row = nlohmann::json::object();
        std::size_t aggregate = 0;
        for (const GroupedItem& item : statement.items) {
            if (!item.aggregate) {
                row[item.alias.empty() ? item.column : item.alias] = group.key;
                continue;
            }
            row[aggregate_output_name(*item.aggregate, item.alias)] =
                finish_aggregate(item.aggregate->func, group.aggregates[aggregate]);
            ++aggregate;
        }
        outcome.rows.push_back(std::move(row));
    }
    return outcome;
}

namespace {

// Merge into an outcome that already carries this coordinator's provenance.
void merge_into(SelectOutcome& outcome, const SelectStmt& statement,
                const std::vector<PlannedPartition>& planned,
                const std::vector<PartitionScanResponse>& answers) {
    std::vector<std::string> order;
    order.reserve(planned.size());
    for (const PlannedPartition& partition : planned) order.push_back(partition.id);
    SelectOutcome merged = merge_scan_responses(statement, order, answers);
    outcome.columns = std::move(merged.columns);
    outcome.rows = std::move(merged.rows);
    outcome.table_columns = std::move(merged.table_columns);
    outcome.table_columns_complete = merged.table_columns_complete;
}

}  // namespace

SelectOutcome DistributedSelect::execute(const SelectStmt& statement,
                                         const std::string& table_id,
                                         std::chrono::milliseconds timeout) {
    SelectOutcome outcome;
    if (statement.aggregating()) {
        if (statement.group_by.size() > 1) {
            outcome.error = "GROUP BY supports exactly one key column";
            return outcome;
        }
        outcome.columns = grouped_columns(statement);
    }

    refresh();  // the catalog may have moved since the last tick

    const std::vector<PlannedPartition> planned = plan_partitions(node_, table_id);
    // Before anything is scattered: is this node's view of the catalog whole?
    // A missing entry is invisible, so it must be caught by counting, not by
    // failing to reach an owner.
    if (std::string reason =
            catalog_completeness_error(node_, table_id, planned.size());
        !reason.empty()) {
        outcome.error = std::move(reason);
        return outcome;
    }
    if (planned.empty()) {  // no placed partitions: an empty dataset
        merge_into(outcome, statement, planned, {});
        return outcome;
    }

    const std::string& self = cluster_.node_id();
    std::unordered_set<std::string> reachable{self};
    for (const zetta::cluster::ClusterPeer& peer : cluster_.peers()) {
        reachable.insert(peer.node_id);
    }
    std::map<std::string, std::vector<std::string>> by_node;
    if (std::string reason = assign_responders(planned, self, reachable, by_node);
        !reason.empty()) {
        outcome.error = std::move(reason);
        return outcome;
    }

    const PartitionScanRequest base = scan_request_for(statement, table_id);

    std::vector<PartitionScanResponse> answers;
    for (const auto& [node_id, partitions] : by_node) {
        PartitionScanRequest request = base;
        request.partitions = partitions;
        if (node_id == self) {
            std::vector<OwnedPartition> owned;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto found = owned_.find(table_id);
                if (found != owned_.end()) owned = found->second;
            }
            const auto started = std::chrono::steady_clock::now();
            PartitionScanResponse response = scan_owned_partitions(owned, request);
            if (!response.ok()) {
                outcome.error = "sql.scan local: " + response.error;
                return outcome;
            }
            response.from = self;
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            outcome.contributions.push_back(
                SelectContribution{self, /*local=*/true, response.partitions_read,
                                   response.rows_scanned, elapsed_ms, 0});
            outcome.partitions_read += response.partitions_read;
            outcome.rows_scanned += response.rows_scanned;
            answers.push_back(std::move(response));
            continue;
        }

        const std::string encoded = encode_partition_scan_request(request);
        const auto started = std::chrono::steady_clock::now();
        const zetta::cluster::SignalResponse reply =
            cluster_.request(node_id, std::string(kScanMethod), encoded, timeout);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        if (!reply.ok) {
            outcome.error = "sql.scan to owner '" + node_id + "' failed: " + reply.error;
            outcome.contributions.clear();
            outcome.rows.clear();
            return outcome;
        }
        std::string decode_error;
        const std::optional<PartitionScanResponse> response =
            decode_partition_scan_response(reply.payload, &decode_error);
        if (!response) {
            outcome.error = "sql.scan answer from owner '" + node_id +
                            "' is malformed: " + decode_error;
            outcome.contributions.clear();
            outcome.rows.clear();
            return outcome;
        }
        outcome.contributions.push_back(SelectContribution{
            node_id, /*local=*/false, response->partitions_read,
            response->rows_scanned, elapsed_ms,
            encoded.size() + reply.payload.size()});
        outcome.partitions_read += response->partitions_read;
        outcome.rows_scanned += response->rows_scanned;
        answers.push_back(*response);
    }

    merge_into(outcome, statement, planned, answers);
    return outcome;
}

SelectOutcome select_over_partitions(Node& node, DistributedSelect* plane,
                                     const SelectStmt& statement,
                                     const std::string& table_id) {
    if (plane != nullptr) return plane->execute(statement, table_id);

    // Standalone: the same plan, with this node as the only reachable owner. A
    // partition placed elsewhere fails exactly as an unreachable peer would.
    SelectOutcome outcome;
    if (statement.aggregating()) {
        if (statement.group_by.size() > 1) {
            outcome.error = "GROUP BY supports exactly one key column";
            return outcome;
        }
        outcome.columns = grouped_columns(statement);
    }
    const std::vector<PlannedPartition> planned = plan_partitions(node, table_id);
    // The same completeness check the clustered path runs: a standalone node's
    // catalog can be short too (a partition it never received, an entry dropped
    // under it), and answering from what is left is the outcome #837 closes.
    if (std::string reason =
            catalog_completeness_error(node, table_id, planned.size());
        !reason.empty()) {
        outcome.error = std::move(reason);
        return outcome;
    }
    if (planned.empty()) {
        merge_into(outcome, statement, planned, {});
        return outcome;
    }

    const std::string& self = node.node_id();
    std::map<std::string, std::vector<std::string>> by_node;
    if (std::string reason =
            assign_responders(planned, self, std::unordered_set<std::string>{self},
                              by_node);
        !reason.empty()) {
        outcome.error = std::move(reason);
        return outcome;
    }

    // Every partition resolved to this node, so there is one entry to serve.
    std::vector<OwnedPartition> owned;
    std::unordered_map<std::string, std::shared_ptr<zetta::data::Database>> databases;
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database = node.find_database(name);
        if (!database) continue;
        for (const std::shared_ptr<zetta::data::Table>& table : database->tables()) {
            if (table && table->id() == table_id) databases.emplace(table_id, database);
        }
    }
    const auto database = databases.find(table_id);
    if (database == databases.end()) {
        outcome.error = "table '" + table_id + "' is not on this node";
        return outcome;
    }
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) continue;
        if (partition->blob_ref().empty()) continue;
        owned.push_back(OwnedPartition{
            partition->id(), partition->row_range().start,
            database->second->large_objects().body_path(partition->blob_ref())});
    }

    PartitionScanRequest request = scan_request_for(statement, table_id);
    request.partitions = by_node[self];
    PartitionScanResponse response = scan_owned_partitions(owned, request);
    if (!response.ok()) {
        outcome.error = "sql.scan local: " + response.error;
        return outcome;
    }
    response.from = self;
    outcome.partitions_read = response.partitions_read;
    outcome.rows_scanned = response.rows_scanned;
    outcome.contributions.push_back(SelectContribution{
        self, /*local=*/true, response.partitions_read, response.rows_scanned, 0.0, 0});
    merge_into(outcome, statement, planned, {response});
    return outcome;
}

}  // namespace zetta::query
