#include "zetta/data/dataset_path.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::data {
namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

// Case-insensitive lookup of `wanted` among `columns`, returning the column's real
// spelling (a warehouse round-trips `CHURNED` and `churned` interchangeably; the
// engine must use the name the table actually carries).
std::string find_column(const std::vector<std::string>& columns, std::string_view wanted) {
    const std::string target = lowered(wanted);
    for (const std::string& column : columns) {
        if (lowered(column) == target) return column;
    }
    return {};
}

// The rows' value for `column`, skipping rows that do not carry it. A connector
// table is schemaless at the row level, so a column may be absent from some rows.
std::vector<const nlohmann::json*> values_of(const std::vector<nlohmann::json>& sample,
                                             const std::string& column) {
    std::vector<const nlohmann::json*> values;
    for (const nlohmann::json& row : sample) {
        if (!row.is_object()) continue;
        const auto found = row.find(column);
        if (found == row.end() || found->is_null()) continue;
        values.push_back(&*found);
    }
    return values;
}

bool is_binary_value(const nlohmann::json& value) {
    if (value.is_boolean()) return true;
    if (value.is_number_integer()) {
        const std::int64_t number = value.get<std::int64_t>();
        return number == 0 || number == 1;
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        return number == 0.0 || number == 1.0;
    }
    return false;
}

bool is_integer_value(const nlohmann::json& value) {
    if (value.is_number_integer()) return true;
    if (value.is_boolean()) return true;
    // A float that happens to be whole is still an integer key — warehouses hand
    // back NUMBER columns as doubles, and refusing them would fail on the most
    // common warehouse shape there is.
    if (value.is_number_float()) {
        const double number = value.get<double>();
        return number == static_cast<double>(static_cast<std::int64_t>(number));
    }
    return false;
}

bool is_text_value(const nlohmann::json& value) {
    return value.is_string() && !value.get_ref<const std::string&>().empty();
}

bool is_numeric_value(const nlohmann::json& value) {
    return value.is_number() || value.is_boolean();
}

// Whether EVERY sampled value of `column` satisfies `predicate`. A column with no
// sampled values at all fails: an empty sample says nothing, and treating silence
// as agreement is how an inference becomes a guess.
bool all_values(const std::vector<nlohmann::json>& sample, const std::string& column,
                bool (*predicate)(const nlohmann::json&)) {
    const std::vector<const nlohmann::json*> values = values_of(sample, column);
    if (values.empty()) return false;
    return std::all_of(values.begin(), values.end(),
                       [predicate](const nlohmann::json* value) { return predicate(*value); });
}

// The four questions inference asks of a column's values.
enum class ValueTest { Binary, Integer, Text, Numeric };

bool (*predicate_of(ValueTest test))(const nlohmann::json&) {
    switch (test) {
        case ValueTest::Binary: return is_binary_value;
        case ValueTest::Integer: return is_integer_value;
        case ValueTest::Text: return is_text_value;
        case ValueTest::Numeric: return is_numeric_value;
    }
    return is_numeric_value;
}

// What a catalog type answers for `test`, or nothing when only values can.
// The answers are the ones the values would give: a `Boolean` column's values are
// all binary, integer and numeric; an `Integer` column's are integer and numeric
// but not known to be 0/1; a `Number` column's are numeric but not known to be
// whole; no value of a numeric column is text; a `Null` column has no values, so
// every test fails, as it does on an empty sample.
std::optional<bool> catalog_answer(CatalogColumnType type, ValueTest test) {
    switch (type) {
        case CatalogColumnType::Boolean: return test != ValueTest::Text;
        case CatalogColumnType::Integer:
            if (test == ValueTest::Binary) return std::nullopt;
            return test != ValueTest::Text;
        case CatalogColumnType::Number:
            if (test == ValueTest::Binary || test == ValueTest::Integer) return std::nullopt;
            return test == ValueTest::Numeric;
        case CatalogColumnType::Null: return false;
        case CatalogColumnType::Undecided: return std::nullopt;
    }
    return std::nullopt;
}

// A column's types, answered from the partition catalog where it decides them and
// from the sampled rows otherwise. Without a catalog it is the sample alone, which
// is how every contract was typed before #1203 item 3.
class ColumnTyping {
public:
    ColumnTyping(const std::vector<nlohmann::json>& sample, const CatalogColumnTypes* catalog)
        : sample_(sample),
          catalog_(catalog != nullptr && catalog->available() ? catalog : nullptr) {}

    // Whether anything can be said about the columns' types at all.
    bool can_type() const { return catalog_ != nullptr || !sample_.empty(); }

    bool all(const std::string& column, ValueTest test) const {
        if (const std::optional<bool> decided = from_catalog(column, test)) return *decided;
        return all_values(sample_, column, predicate_of(test));
    }

    bool has_values(const std::string& column) const {
        if (catalog_ != nullptr) {
            switch (catalog_->type_of(column)) {
                case CatalogColumnType::Null: return false;
                case CatalogColumnType::Boolean:
                case CatalogColumnType::Integer:
                case CatalogColumnType::Number: return true;
                case CatalogColumnType::Undecided: break;
            }
        }
        return !values_of(sample_, column).empty();
    }

    // Where `test`'s answer for `column` came from, for the note that reports it.
    std::string basis(const std::string& column, ValueTest test) const {
        if (from_catalog(column, test)) {
            return "by its type in the partition catalog, " +
                   std::string(to_string(catalog_->type_of(column)));
        }
        return "across the sampled rows";
    }

private:
    std::optional<bool> from_catalog(const std::string& column, ValueTest test) const {
        if (catalog_ == nullptr) return std::nullopt;
        return catalog_answer(catalog_->type_of(column), test);
    }

    const std::vector<nlohmann::json>& sample_;
    const CatalogColumnTypes* catalog_;
};

// One recorded type, from a Parquet body (its physical type) or a JSON Lines body
// (the JSON family its values share).
CatalogColumnType recorded_type(std::string_view recorded) {
    if (recorded == "boolean") return CatalogColumnType::Boolean;
    if (recorded == "int32" || recorded == "int64" || recorded == "integer") {
        return CatalogColumnType::Integer;
    }
    if (recorded == "float" || recorded == "double" || recorded == "number") {
        return CatalogColumnType::Number;
    }
    if (recorded == "null") return CatalogColumnType::Null;
    // "byte_array" (text, or a column with no value in the body), "string",
    // "mixed", "object", "array", anything newer: only values can say.
    return CatalogColumnType::Undecided;
}

CatalogColumnType merge_types(CatalogColumnType left, CatalogColumnType right) {
    if (left == right) return left;
    if (left == CatalogColumnType::Null) return right;
    if (right == CatalogColumnType::Null) return left;
    if (left == CatalogColumnType::Undecided || right == CatalogColumnType::Undecided) {
        return CatalogColumnType::Undecided;
    }
    // Two different numeric types: a real anywhere makes the column real, and a
    // boolean partition beside an integer one is integer (true/false read as 1/0).
    if (left == CatalogColumnType::Number || right == CatalogColumnType::Number) {
        return CatalogColumnType::Number;
    }
    return CatalogColumnType::Integer;
}

// Whether `name` can be written as a column in the SQL subset's select list: it
// has no quoted identifiers, so the name must lex as one bare identifier and must
// not be a word the parser reads as something else (`true`, `null`, `limit`, …).
bool plain_identifier(const std::string& name) {
    if (name.empty()) return false;
    if (std::isalpha(static_cast<unsigned char>(name.front())) == 0 && name.front() != '_') {
        return false;
    }
    for (const char character : name) {
        if (std::isalnum(static_cast<unsigned char>(character)) == 0 && character != '_') {
            return false;
        }
    }
    static const std::vector<std::string> reserved = {
        "all",    "and",     "any",   "as",     "asc",     "between", "by",     "case",
        "count",  "create",  "delete", "desc",  "distinct", "drop",   "else",   "end",
        "false",  "from",    "group", "having", "in",      "insert",  "is",     "join",
        "like",   "limit",   "not",   "null",   "offset",  "on",      "or",     "order",
        "over",   "select",  "set",   "table",  "then",    "true",    "union",  "update",
        "using",  "values",  "when",  "where",  "with",    "sum",     "avg",    "min",
        "max"};
    return std::ranges::find(reserved, lowered(name)) == reserved.end();
}

// ` LIMIT <limit>`, or nothing for a zero limit (every row). Without ORDER BY the
// rows kept are the first ones in scan order, the same rows truncating a full read
// keeps; with the LIMIT pushed into the scan (#1021) only those rows are read.
std::string limit_clause(std::size_t limit) {
    return limit == 0 ? std::string() : " LIMIT " + std::to_string(limit);
}

// Keep only `columns` of each row.
std::vector<nlohmann::json> project_rows(std::vector<nlohmann::json> rows,
                                         const std::vector<std::string>& columns) {
    for (nlohmann::json& row : rows) {
        if (!row.is_object()) continue;
        nlohmann::json kept = nlohmann::json::object();
        for (const std::string& column : columns) {
            const auto found = row.find(column);
            if (found != row.end()) kept[column] = std::move(*found);
        }
        row = std::move(kept);
    }
    return rows;
}

std::string join(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) out += separator;
        out += items[index];
    }
    return out;
}

std::string categorical_note_prefix(const std::string& column) {
    return "categorical column: '" + column + "'";
}

std::string categorical_note(const CategoricalEncoding& encoding, const char* rows) {
    std::string levels;
    for (std::size_t index = 0; index < encoding.levels.size(); ++index) {
        if (index != 0) levels += ", ";
        levels += encoding.levels[index];
    }
    return categorical_note_prefix(encoding.column) + " (text, " +
           std::to_string(encoding.levels.size()) + " levels in the " + rows + ": " +
           levels + "; one-hot encoded as " + encoding.column +
           "=<level> where the trainer reads categorical columns, the GBDT (M-1) "
           "does; an unseen or missing value encodes as all zeros)";
}

// Fit `column` as a category, refusing an encoding whose feature names would
// collide with a real column of the table.
CategoricalFit fit_category(const std::string& column, const std::vector<std::string>& columns,
                            const std::vector<nlohmann::json>& rows) {
    CategoricalFit fit = fit_categorical_encoding(column, rows);
    if (!fit.ok()) return fit;
    for (const std::string& feature : fit.encoding->feature_names()) {
        if (!find_column(columns, feature).empty()) {
            CategoricalFit refused;
            refused.reason = "its encoded feature '" + feature +
                             "' would collide with a column of the same name";
            return refused;
        }
    }
    return fit;
}

}  // namespace

std::optional<std::int64_t> EntityKeyMapping::derived(
    std::string_view original) const {
    const auto found = original_to_derived.find(std::string(original));
    if (found == original_to_derived.end()) {
        return std::nullopt;
    }
    return found->second;
}

nlohmann::json EntityKeyMapping::original(std::int64_t derived_id) const {
    const auto found = derived_to_original.find(derived_id);
    if (found == derived_to_original.end()) {
        return derived_id;
    }
    return found->second;
}

nlohmann::json EntityKeyMapping::to_json() const {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [derived_id, original_id] : derived_to_original) {
        entries.push_back({{"derived_id", derived_id},
                           {"original_value", original_id}});
    }
    return {{"column", column}, {"entries", std::move(entries)}};
}

EntityKeyMappingResult derive_entity_key_mapping(
    std::string column, const std::vector<nlohmann::json>& values,
    const EntityKeyHash& hash) {
    EntityKeyMappingResult result;
    auto mapping = std::make_shared<EntityKeyMapping>();
    mapping->column = std::move(column);
    const auto stable_hash = [&hash](std::string_view value) {
        if (hash) {
            return hash(value);
        }
        std::uint64_t derived = UINT64_C(14695981039346656037);
        for (const unsigned char character : value) {
            derived ^= character;
            derived *= UINT64_C(1099511628211);
        }
        return derived;
    };
    for (const nlohmann::json& value : values) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            result.error = "dataset: the entity column '" + mapping->column +
                           "' mixes text with non-text or empty values";
            return result;
        }
        const std::string original = value.get<std::string>();
        if (mapping->original_to_derived.contains(original)) {
            continue;
        }
        // Several native aggregation paths widen exact integer keys to double.
        // Keep the derived id inside double's exact-integer range so grouping,
        // joins and later int64 restoration preserve the same key bit-for-bit.
        constexpr std::uint64_t kExactIntegerMask =
            (UINT64_C(1) << 53) - UINT64_C(1);
        const std::int64_t derived_id = static_cast<std::int64_t>(
            stable_hash(original) & kExactIntegerMask);
        const auto collision = mapping->derived_to_original.find(derived_id);
        if (collision != mapping->derived_to_original.end() &&
            collision->second != original) {
            result.error =
                "dataset: derived entity key collision in column '" +
                mapping->column + "': '" + collision->second + "' and '" +
                original + "' map to " + std::to_string(derived_id);
            return result;
        }
        mapping->derived_to_original.emplace(derived_id, original);
        mapping->original_to_derived.emplace(original, derived_id);
    }
    if (mapping->empty()) {
        result.error = "dataset: the text entity column '" + mapping->column +
                       "' has no values to key";
        return result;
    }
    result.mapping = std::move(mapping);
    return result;
}

nlohmann::json entity_key_mapping_summary(const nlohmann::json& mapping) {
    nlohmann::json summary = nlohmann::json::object();
    if (const auto column = mapping.find("column"); column != mapping.end()) {
        summary["column"] = *column;
    }
    const auto entries = mapping.find("entries");
    summary["entry_count"] =
        entries != mapping.end() && entries->is_array() ? entries->size() : 0;
    return summary;
}

std::size_t summarize_entity_key_mappings(nlohmann::json& document) {
    std::size_t replaced = 0;
    if (document.is_object()) {
        for (auto& [key, value] : document.items()) {
            if (key == "entity_key_mapping" && value.is_object() &&
                value.contains("entries")) {
                value = entity_key_mapping_summary(value);
                ++replaced;
            } else {
                replaced += summarize_entity_key_mappings(value);
            }
        }
    } else if (document.is_array()) {
        for (nlohmann::json& item : document) {
            replaced += summarize_entity_key_mappings(item);
        }
    }
    return replaced;
}

const nlohmann::json& entity_key_mapping_summary_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"column", "entry_count"}},
        {"additionalProperties", false},
        {"description",
         "the text entity column and how many distinct ids it maps; the "
         "entries themselves only on request (include_entity_keys)"},
        {"properties",
         {{"column", {{"type", "string"}, {"minLength", 1}}},
          {"entry_count", {{"type", "integer"}, {"minimum", 0}}}}}};
    return schema;
}

std::shared_ptr<const EntityKeyMapping> entity_key_mapping_from_json(
    const nlohmann::json& value, std::string* error) {
    const auto fail = [error](std::string reason) {
        if (error != nullptr) {
            *error = std::move(reason);
        }
        return std::shared_ptr<const EntityKeyMapping>{};
    };
    if (!value.is_object() || !value.contains("column") ||
        !value.at("column").is_string() ||
        value.at("column").get_ref<const std::string&>().empty() ||
        !value.contains("entries") || !value.at("entries").is_array()) {
        return fail("entity key mapping requires a column and entries array");
    }
    auto mapping = std::make_shared<EntityKeyMapping>();
    mapping->column = value.at("column").get<std::string>();
    for (const nlohmann::json& entry : value.at("entries")) {
        if (!entry.is_object() || !entry.contains("derived_id") ||
            !entry.at("derived_id").is_number_integer() ||
            !entry.contains("original_value") ||
            !entry.at("original_value").is_string() ||
            entry.at("original_value").get_ref<const std::string&>().empty()) {
            return fail("entity key mapping entry is malformed");
        }
        const std::int64_t derived_id =
            entry.at("derived_id").get<std::int64_t>();
        const std::string original =
            entry.at("original_value").get<std::string>();
        if (!mapping->derived_to_original.emplace(derived_id, original).second ||
            !mapping->original_to_derived.emplace(original, derived_id).second) {
            return fail("entity key mapping contains a duplicate key or value");
        }
    }
    if (mapping->empty()) {
        return fail("entity key mapping is empty");
    }
    return mapping;
}

nlohmann::json entity_output(const DatasetContract& contract,
                             std::int64_t derived) {
    if (!contract.entity_keys) {
        return derived;
    }
    return contract.entity_keys->original(derived);
}

bool DatasetContract::operator==(const DatasetContract& other) const {
    const bool mappings_equal =
        (!entity_keys && !other.entity_keys) ||
        (entity_keys && other.entity_keys &&
         entity_keys->column == other.entity_keys->column &&
         entity_keys->derived_to_original ==
             other.entity_keys->derived_to_original);
    return entity_column == other.entity_column &&
           label_column == other.label_column &&
           time_column == other.time_column && as_of == other.as_of &&
           feature_columns == other.feature_columns &&
           categorical_features == other.categorical_features && mappings_equal;
}

std::string_view to_string(DatasetPathKind kind) {
    switch (kind) {
        case DatasetPathKind::Directory: return "directory";
        case DatasetPathKind::Table: return "table";
    }
    return "directory";
}

std::string_view to_string(CatalogColumnType type) {
    switch (type) {
        case CatalogColumnType::Boolean: return "boolean";
        case CatalogColumnType::Integer: return "integer";
        case CatalogColumnType::Number: return "number";
        case CatalogColumnType::Null: return "null";
        case CatalogColumnType::Undecided: return "undecided";
    }
    return "undecided";
}

CatalogColumnType CatalogColumnTypes::type_of(const std::string& column) const {
    const auto found = columns.find(column);
    return found == columns.end() ? CatalogColumnType::Undecided : found->second;
}

CatalogColumnTypes catalog_column_types(
    const std::vector<std::shared_ptr<DatasetPartition>>& partitions,
    const std::string& table_id, std::uint64_t rows) {
    CatalogColumnTypes catalog;
    std::uint64_t covered = 0;
    for (const std::shared_ptr<DatasetPartition>& partition : partitions) {
        if (!partition || partition->table() != table_id) continue;
        ++catalog.partitions;
        const PartitionRowRange& range = partition->row_range();
        if (range.end > range.start) covered += range.end - range.start;
        const nlohmann::json& stats = partition->stats();
        const nlohmann::json* schema = nullptr;
        if (stats.is_object()) {
            if (const auto found = stats.find("schema");
                found != stats.end() && found->is_array()) {
                schema = &*found;
            }
        }
        if (schema == nullptr) {
            catalog.unavailable = "partition " + partition->id() +
                                  " records no column types in the catalog";
            catalog.columns.clear();
            return catalog;
        }
        for (const nlohmann::json& column : *schema) {
            if (!column.is_object() || !column.contains("name") ||
                !column.at("name").is_string() || !column.contains("type") ||
                !column.at("type").is_string()) {
                continue;
            }
            const std::string& name = column.at("name").get_ref<const std::string&>();
            const CatalogColumnType type =
                recorded_type(column.at("type").get_ref<const std::string&>());
            const auto [slot, inserted] = catalog.columns.emplace(name, type);
            if (!inserted) slot->second = merge_types(slot->second, type);
        }
    }
    if (catalog.partitions == 0) {
        catalog.unavailable = "the table has no partitions in the catalog";
    } else if (covered != rows) {
        catalog.unavailable = "the partition catalog covers " + std::to_string(covered) +
                              " of the table's " + std::to_string(rows) + " rows";
    }
    if (!catalog.available()) catalog.columns.clear();
    return catalog;
}

std::vector<std::string> columns_needing_values(const std::vector<std::string>& columns,
                                                const CatalogColumnTypes& catalog,
                                                const DatasetContract& overrides) {
    const auto named = [](const std::string& column, const std::string& wanted) {
        return !wanted.empty() && lowered(column) == lowered(wanted);
    };
    const auto candidate = [](const std::string& column,
                              const std::vector<std::string>& candidates) {
        const std::string name = lowered(column);
        return std::ranges::any_of(candidates, [&name](const std::string& wanted) {
            return lowered(wanted) == name;
        });
    };
    std::vector<std::string> needed;
    for (const std::string& column : columns) {
        const CatalogColumnType type = catalog.type_of(column);
        // The catalog says a column is numeric but not whether it is 0/1, which is
        // what makes a label candidate a label.
        const bool label_values =
            overrides.label_column.empty() && candidate(column, label_column_candidates()) &&
            (type == CatalogColumnType::Integer || type == CatalogColumnType::Number);
        // Nor whether reals are whole, which is what makes a number an entity key.
        const bool entity_values =
            type == CatalogColumnType::Number &&
            (named(column, overrides.entity_column) ||
             (overrides.entity_column.empty() &&
              candidate(column, entity_column_candidates())));
        if (type == CatalogColumnType::Undecided || label_values || entity_values) {
            needed.push_back(column);
        }
    }
    return needed;
}

const std::vector<std::string>& entity_column_candidates() {
    // Most specific first: a table carrying both `account_id` and `id` means the
    // account, not the row.
    static const std::vector<std::string> candidates = {
        "entity",      "entity_id",  "account_id", "customer_id", "subscriber_id",
        "customer_key", "user_id",   "member_id",  "client_id",   "org_id",
        "id",
    };
    return candidates;
}

const std::vector<std::string>& label_column_candidates() {
    static const std::vector<std::string> candidates = {
        "churned",  "churn",     "is_churn", "is_churned", "did_churn",
        "label",    "target",    "outcome",  "converted",  "y",
    };
    return candidates;
}

DatasetPath parse_dataset_path(std::string_view path, const std::string& default_database) {
    DatasetPath parsed;

    // The separator test comes FIRST because a path is the unambiguous form: a
    // directory may legitimately contain a dot (`./data/churn.v2`), while a table
    // name may not contain a slash.
    const bool looks_like_a_path =
        path.find('/') != std::string_view::npos || path.find('\\') != std::string_view::npos ||
        path.starts_with(".") || path.starts_with("~");
    if (looks_like_a_path) {
        parsed.kind = DatasetPathKind::Directory;
        parsed.directory = std::filesystem::path(path);
        return parsed;
    }

    // A bare name is always a table. The grammar never asks the filesystem (#1433):
    // an existence test here answered "is there a file called X in the node's
    // working directory" to any caller, and a directory under the import root is
    // named with a separator (`./churn`) anyway.

    parsed.kind = DatasetPathKind::Table;
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        parsed.database = default_database;
        parsed.table = std::string(path);
    } else {
        parsed.database = std::string(path.substr(0, dot));
        parsed.table = std::string(path.substr(dot + 1));
    }
    return parsed;
}

DatasetContractResult infer_dataset_contract(const std::vector<std::string>& columns,
                                             const std::vector<nlohmann::json>& sample,
                                             const DatasetContract& overrides,
                                             const CatalogColumnTypes* catalog) {
    DatasetContractResult result;
    result.contract = overrides;
    const ColumnTyping typing(sample, catalog);

    const auto fail = [&result](std::string reason) -> DatasetContractResult {
        result.error = std::move(reason);
        return result;
    };
    if (columns.empty()) return fail("dataset: the table has no columns");

    // An explicitly named column that is not there is a typo, not an intent — say so
    // rather than falling back to inference and training on something else.
    const auto require_present = [&columns](const std::string& named,
                                            const char* role) -> std::string {
        if (named.empty()) return {};
        if (!find_column(columns, named).empty()) return {};
        return std::string("dataset: no column '") + named + "' for the " + role +
               " (the table has: " + join(columns, ", ") + ")";
    };
    for (const auto& [named, role] :
         {std::pair{overrides.entity_column, "entity"},
          std::pair{overrides.label_column, "label"},
          std::pair{overrides.time_column, "time cutoff"}}) {
        if (std::string reason = require_present(named, role); !reason.empty()) {
            return fail(std::move(reason));
        }
    }
    for (const std::string& named : overrides.feature_columns) {
        if (std::string reason = require_present(named, "feature"); !reason.empty()) {
            return fail(std::move(reason));
        }
    }

    // Normalize the caller's spellings to the table's own.
    if (!result.contract.entity_column.empty()) {
        result.contract.entity_column = find_column(columns, result.contract.entity_column);
    }
    if (!result.contract.label_column.empty()) {
        result.contract.label_column = find_column(columns, result.contract.label_column);
    }
    if (!result.contract.time_column.empty()) {
        result.contract.time_column = find_column(columns, result.contract.time_column);
    }

    // --- the label ----------------------------------------------------------
    // Binary-ness outranks the name: a column called `label` holding free text is
    // not a training target, and training on it would fail deep inside the GBDT
    // rather than here, where the reason is still legible.
    if (result.contract.label_column.empty()) {
        std::vector<std::string> named_but_not_binary;
        for (const std::string& candidate : label_column_candidates()) {
            const std::string column = find_column(columns, candidate);
            if (column.empty()) continue;
            if (typing.all(column, ValueTest::Binary)) {
                result.contract.label_column = column;
                result.notes.push_back("label column: '" + column + "' (binary " +
                                       typing.basis(column, ValueTest::Binary) + ")");
                break;
            }
            named_but_not_binary.push_back(column);
        }
        if (result.contract.label_column.empty()) {
            std::string reason =
                "dataset: no label column found — looked for " +
                join(label_column_candidates(), " / ") +
                " holding 0/1 values. Name one with the dataset contract's "
                "label_column.";
            if (!named_but_not_binary.empty()) {
                reason += " (" + join(named_but_not_binary, ", ") +
                          " matched by name but is not binary numeric data in the "
                          "sampled rows; cast it to 0/1 before running, for example " +
                          named_but_not_binary.front() + "::int)";
            }
            return fail(std::move(reason));
        }
    } else if (typing.can_type() &&
               !typing.all(result.contract.label_column, ValueTest::Numeric)) {
        return fail("dataset: the label column '" +
                    result.contract.label_column +
                    "' is non-numeric; cast it to a numeric type before "
                    "running (for SQL, use " + result.contract.label_column +
                    "::int for a binary label)");
    }

    // --- the entity ---------------------------------------------------------
    // Numeric algorithms still use int64 keys. Text business identifiers get a
    // deterministic dataset-scoped key and are restored at every output boundary.
    if (result.contract.entity_column.empty()) {
        std::vector<std::string> named_but_not_keyable;
        for (const std::string& candidate : entity_column_candidates()) {
            const std::string column = find_column(columns, candidate);
            if (column.empty() || column == result.contract.label_column) {
                continue;
            }
            if (typing.all(column, ValueTest::Integer)) {
                result.contract.entity_column = column;
                result.notes.push_back("entity column: '" + column + "' (integer " +
                                       typing.basis(column, ValueTest::Integer) + ")");
                break;
            }
            if (typing.all(column, ValueTest::Text)) {
                std::vector<nlohmann::json> values;
                for (const nlohmann::json* value : values_of(sample, column)) {
                    values.push_back(*value);
                }
                EntityKeyMappingResult mapped =
                    derive_entity_key_mapping(column, values);
                if (!mapped.ok()) {
                    return fail(std::move(mapped.error));
                }
                result.contract.entity_column = column;
                result.contract.entity_keys = std::move(mapped.mapping);
                result.notes.push_back(
                    "entity column '" + column +
                    "' (text; keyed by a derived id, outputs carry the original "
                    "value)");
                break;
            }
            named_but_not_keyable.push_back(column);
        }
        if (result.contract.entity_column.empty()) {
            std::string reason = "dataset: no keyable entity column found — looked for " +
                                 join(entity_column_candidates(), " / ") + ".";
            if (!named_but_not_keyable.empty()) {
                reason += " " + join(named_but_not_keyable, ", ") +
                          " matched by name but contains values that are neither "
                          "integers nor text.";
            } else {
                reason += " Name one with the dataset contract's entity_column.";
            }
            return fail(std::move(reason));
        }
    } else if (typing.can_type()) {
        if (typing.all(result.contract.entity_column, ValueTest::Integer)) {
            result.notes.push_back(
                "entity column: '" + result.contract.entity_column + "' (integer " +
                typing.basis(result.contract.entity_column, ValueTest::Integer) + ")");
        } else if (typing.all(result.contract.entity_column, ValueTest::Text)) {
            std::vector<nlohmann::json> values;
            for (const nlohmann::json* value :
                 values_of(sample, result.contract.entity_column)) {
                values.push_back(*value);
            }
            EntityKeyMappingResult mapped = derive_entity_key_mapping(
                result.contract.entity_column, values);
            if (!mapped.ok()) {
                return fail(std::move(mapped.error));
            }
            result.contract.entity_keys = std::move(mapped.mapping);
            result.notes.push_back(
                "entity column '" + result.contract.entity_column +
                "' (text; keyed by a derived id, outputs carry the original value)");
        } else {
            return fail("dataset: the entity column '" +
                        result.contract.entity_column +
                        "' contains values that are neither integers nor text");
        }
    }

    // --- the features -------------------------------------------------------
    // Fitted afresh: a contract carried in from a previous resolution must not
    // smuggle its levels past the rows this one reads.
    result.contract.categorical_features.clear();
    if (result.contract.feature_columns.empty()) {
        for (const std::string& column : columns) {
            if (column == result.contract.entity_column) {
                continue;
            }
            if (column == result.contract.label_column) {
                continue;
            }
            if (column == result.contract.time_column) {
                continue;
            }
            if (typing.all(column, ValueTest::Numeric)) {
                result.contract.feature_columns.push_back(column);
                continue;
            }
            // Said out loud either way: a column left out silently is one the caller
            // assumes the model used (lake's UX review 2026-09-27).
            if (!typing.has_values(column)) {
                result.notes.push_back("not used: '" + column +
                                       "' (no values in the sampled rows)");
                continue;
            }
            CategoricalFit fit = fit_category(column, columns, sample);
            if (!fit.ok()) {
                result.notes.push_back("not used: '" + column + "' (not numeric, and " +
                                       fit.reason + ")");
                continue;
            }
            result.notes.push_back(categorical_note(*fit.encoding, "sampled rows"));
            result.contract.categorical_features.push_back(std::move(*fit.encoding));
        }
        if (result.contract.feature_columns.empty() &&
            result.contract.categorical_features.empty()) {
            return fail(
                "dataset: no numeric or categorical feature columns beside the entity and "
                "label — a label with no features is not a training set. Add engineered "
                "numeric columns to the query, or name them with the dataset contract's "
                "feature_columns.");
        }
        if (!result.contract.feature_columns.empty()) {
            result.notes.push_back(
                "feature columns (" +
                std::to_string(result.contract.feature_columns.size()) +
                "): " + join(result.contract.feature_columns, ", "));
        }
    } else {
        // The caller's own spellings, normalized, with the entity/label kept out —
        // carrying the label through as a feature would leak the answer into X.
        std::vector<std::string> normalized;
        for (const std::string& named : result.contract.feature_columns) {
            const std::string column = find_column(columns, named);
            if (column == result.contract.entity_column ||
                column == result.contract.label_column) {
                return fail("dataset: '" + column +
                            "' cannot be a feature — it is the " +
                            (column == result.contract.label_column ? "label" : "entity") +
                            " column");
            }
            normalized.push_back(column);
            // A named text column stays in `feature_columns` (a methodology that
            // reads text, such as M-10, takes it from there) and is ALSO recorded
            // as a category when it is one, for a trainer that one-hot encodes it.
            if (!typing.can_type() || typing.all(column, ValueTest::Numeric) ||
                !typing.has_values(column)) {
                continue;
            }
            CategoricalFit fit = fit_category(column, columns, sample);
            if (fit.ok()) {
                result.notes.push_back(categorical_note(*fit.encoding, "sampled rows"));
                result.contract.categorical_features.push_back(std::move(*fit.encoding));
            } else {
                result.notes.push_back("named feature '" + column +
                                       "' is not numeric, and is not one-hot encoded: " +
                                       fit.reason);
            }
        }
        result.contract.feature_columns = std::move(normalized);
    }

    // --- the time cutoff ----------------------------------------------------
    // Never inferred. A time column and an as_of decide the leakage boundary, and
    // guessing that would silently decide whether the model is honest.
    if (!result.contract.time_column.empty()) {
        result.notes.push_back("point-in-time cutoff: rows with " +
                               result.contract.time_column + " <= " +
                               std::to_string(result.contract.as_of));
    } else {
        result.notes.push_back(
            "no time column given — the rows are taken as already point-in-time correct");
    }
    return result;
}

DatasetSampler default_dataset_sampler(Node& node, const zetta::cluster::Cluster* cluster) {
    return caller_dataset_sampler(node, cluster, {}, false);
}

DatasetSampler caller_dataset_sampler(Node& node, const zetta::cluster::Cluster* cluster,
                                      std::string acting_key, bool acting_is_user) {
    return [&node, cluster, acting_key = std::move(acting_key), acting_is_user](
               const std::string& database, const std::string& table, std::size_t limit,
               std::string& error) {
        zetta::query::QueryService service(node, cluster, acting_key, acting_is_user);
        const zetta::query::QueryService::Outcome outcome =
            service.run("SELECT * FROM " + database + "." + table + limit_clause(limit),
                        database);
        if (!outcome.ok()) {
            error = outcome.error;
            return std::vector<nlohmann::json>{};
        }
        std::vector<nlohmann::json> rows = outcome.rows;
        if (limit > 0 && rows.size() > limit) {
            rows.resize(limit);
        }
        return rows;
    };
}

std::vector<nlohmann::json> read_dataset_columns(Node& node,
                                                 const zetta::cluster::Cluster* cluster,
                                                 const std::string& database,
                                                 const std::string& table,
                                                 const std::vector<std::string>& columns,
                                                 std::size_t limit, std::string& error) {
    const auto truncated = [limit](std::vector<nlohmann::json> rows) {
        if (limit > 0 && rows.size() > limit) rows.resize(limit);
        return rows;
    };
    if (std::ranges::all_of(columns, plain_identifier)) {
        zetta::query::QueryService service(node, cluster);
        const zetta::query::QueryService::Outcome outcome = service.run(
            "SELECT " + join(columns, ", ") + " FROM " + database + "." + table +
                limit_clause(limit),
            database);
        // The answer must be the named columns and nothing else; anything else (a
        // name the parser read as something other than a column) is not trusted.
        if (outcome.ok() && outcome.columns == columns) {
            return truncated(project_rows(outcome.rows, columns));
        }
    }
    std::string whole_error;
    std::vector<nlohmann::json> rows =
        default_dataset_sampler(node, cluster)(database, table, limit, whole_error);
    if (!whole_error.empty()) {
        error = std::move(whole_error);
        return {};
    }
    return project_rows(std::move(rows), columns);
}

namespace {

// The live dataset `path` names, refused when it is a directory path, missing,
// degraded, or empty. The shared lookup behind sample_dataset and resolve_dataset;
// `sample` is left empty.
SampledDataset locate_dataset(Node& node, const DatasetPath& path) {
    SampledDataset sampled;
    const auto fail = [&sampled](std::string reason) -> SampledDataset {
        sampled.error = std::move(reason);
        return sampled;
    };
    if (path.kind != DatasetPathKind::Table) {
        return fail("dataset: '" + path.directory.string() +
                    "' is a directory path, not a database.table — load it with "
                    "load_churn_dataset instead");
    }
    sampled.database = path.database;
    sampled.table = path.table;

    // The live catalog, recomputed from the node tree joined with the partition
    // catalog — never a stored copy (RULE-7).
    const std::vector<DatasetSummary> datasets = node.datasets();
    const DatasetSummary* found = nullptr;
    std::vector<std::string> near_misses;
    for (const DatasetSummary& summary : datasets) {
        if (lowered(summary.name) == lowered(path.table) &&
            lowered(summary.database) == lowered(path.database)) {
            found = &summary;
            break;
        }
        if (lowered(summary.name) == lowered(path.table)) {
            near_misses.push_back(summary.database + "." + summary.name);
        }
    }
    if (found == nullptr) {
        std::string reason = "dataset: no table '" + path.database + "." + path.table + "'";
        if (!near_misses.empty()) {
            reason += " — did you mean " + join(near_misses, " or ") + "?";
        } else if (!datasets.empty()) {
            std::vector<std::string> available;
            for (const DatasetSummary& summary : datasets) {
                available.push_back(summary.database + "." + summary.name);
            }
            if (available.size() > 12) available.resize(12);
            reason += " (this cluster has: " + join(available, ", ") + ")";
        }
        return fail(std::move(reason));
    }

    // A degraded dataset would type its columns — and then train — on a FRACTION of
    // the rows while looking perfectly healthy. That is the one outcome that must
    // not ship (#837).
    if (found->degraded) {
        return fail("dataset: '" + path.database + "." + path.table +
                    "' is degraded on this node: " + found->degraded_reason +
                    " — training on it would train on part of the data");
    }

    sampled.table_id = found->table_id;
    sampled.database = found->database;
    sampled.table = found->name;
    sampled.rows = found->rows;
    sampled.columns = found->columns;
    if (found->rows == 0) {
        return fail("dataset: '" + path.database + "." + path.table + "' has no rows");
    }
    return sampled;
}

}  // namespace

SampledDataset sample_dataset(Node& node, const DatasetPath& path,
                              const DatasetSampler& sampler,
                              std::size_t sample_rows,
                              const zetta::cluster::Cluster* cluster) {
    SampledDataset sampled = locate_dataset(node, path);
    if (!sampled.ok()) return sampled;
    const auto fail = [&sampled](std::string reason) -> SampledDataset {
        sampled.error = std::move(reason);
        return sampled;
    };
    std::string sample_error;
    const DatasetSampler read = sampler ? sampler : default_dataset_sampler(node, cluster);
    sampled.sample = read(sampled.database, sampled.table, sample_rows, sample_error);
    if (!sample_error.empty()) {
        return fail("dataset: could not read '" + path.database + "." + path.table +
                    "': " + sample_error);
    }
    if (sampled.sample.empty()) {
        return fail("dataset: '" + path.database + "." + path.table +
                    "' returned no rows to type its columns from");
    }
    return sampled;
}

ResolvedDataset resolve_dataset(Node& node, const DatasetPath& path,
                                const DatasetContract& overrides,
                                const DatasetSampler& sampler,
                                std::size_t sample_rows,
                                const zetta::cluster::Cluster* cluster) {
    ResolvedDataset resolved;
    const SampledDataset sampled = locate_dataset(node, path);
    if (!sampled.ok()) {
        resolved.error = sampled.error;
        return resolved;
    }

    resolved.table_id = sampled.table_id;
    resolved.database = sampled.database;
    resolved.table = sampled.table;
    resolved.rows = sampled.rows;
    const std::string qualified = path.database + "." + path.table;

    // Read `columns` of up to `limit` rows (zero: every row). An injected sampler
    // hands back whole rows and they are cut to the columns asked for, so the
    // inference sees the same thing either way.
    const auto read = [&](const std::vector<std::string>& columns, std::size_t limit,
                          std::string& error) {
        if (sampler) {
            return project_rows(sampler(sampled.database, sampled.table, limit, error),
                                columns);
        }
        return read_dataset_columns(node, cluster, sampled.database, sampled.table, columns,
                                    limit, error);
    };

    // --- typing (#1203 item 3) ----------------------------------------------
    // From the partition catalog where it can, reading rows only for the columns
    // whose values decide something the catalog does not record.
    const CatalogColumnTypes catalog =
        catalog_column_types(node.dataset_partitions(), sampled.table_id, sampled.rows);
    DatasetReads& reads = resolved.reads;
    reads.typed_from_catalog = catalog.available();
    reads.catalog_unavailable = catalog.unavailable;
    reads.catalog_partitions = catalog.partitions;
    std::vector<nlohmann::json> sample;
    std::string sample_error;
    if (catalog.available()) {
        reads.sampled_columns = columns_needing_values(sampled.columns, catalog, overrides);
        if (!reads.sampled_columns.empty()) {
            sample = read(reads.sampled_columns, sample_rows, sample_error);
        }
    } else {
        reads.sampled_columns = sampled.columns;
        const DatasetSampler whole = sampler ? sampler : default_dataset_sampler(node, cluster);
        sample = whole(sampled.database, sampled.table, sample_rows, sample_error);
    }
    if (!sample_error.empty()) {
        resolved.error = "dataset: could not read '" + qualified + "': " + sample_error;
        return resolved;
    }
    if (!reads.sampled_columns.empty() && sample.empty()) {
        resolved.error =
            "dataset: '" + qualified + "' returned no rows to type its columns from";
        return resolved;
    }
    reads.sampled_rows = sample.size();

    DatasetContractResult contract = infer_dataset_contract(
        sampled.columns, sample, overrides, catalog.available() ? &catalog : nullptr);
    if (!contract.ok()) {
        resolved.error = contract.error;
        return resolved;
    }
    resolved.contract = std::move(contract.contract);
    resolved.notes = std::move(contract.notes);
    std::string typing_note;
    if (!catalog.available()) {
        typing_note = "column types: from the first " + std::to_string(sample.size()) +
                      " rows, every column (" + catalog.unavailable + ")";
    } else if (reads.sampled_columns.empty()) {
        typing_note = "column types: from the partition catalog (" +
                      std::to_string(catalog.partitions) +
                      " partitions); no rows read to type them";
    } else {
        typing_note = "column types: from the partition catalog (" +
                      std::to_string(catalog.partitions) + " partitions); values of " +
                      join(reads.sampled_columns, ", ") + " read from the first " +
                      std::to_string(sample.size()) +
                      " rows (text, or a label or entity the catalog cannot type)";
    }
    resolved.notes.insert(resolved.notes.begin(), std::move(typing_note));
    if (!resolved.contract.entity_keys && resolved.contract.categorical_features.empty()) {
        return resolved;
    }

    // --- fitting -------------------------------------------------------------
    // A text entity is keyed, and a category's levels are fitted, over EVERY row
    // rather than the sample: a key missing from the map, or a level that was
    // common but unsampled, would otherwise surface only at training time. Only
    // the columns that fitting reads are read: the entity, the categories, and the
    // time column that picks the training rows.
    std::vector<std::string> fit_columns;
    const auto fit_column = [&fit_columns](const std::string& column) {
        if (!column.empty() && std::ranges::find(fit_columns, column) == fit_columns.end()) {
            fit_columns.push_back(column);
        }
    };
    if (resolved.contract.entity_keys) fit_column(resolved.contract.entity_column);
    for (const CategoricalEncoding& category : resolved.contract.categorical_features) {
        fit_column(category.column);
    }
    if (!resolved.contract.categorical_features.empty()) {
        fit_column(resolved.contract.time_column);
    }
    std::string read_error;
    const std::vector<nlohmann::json> rows = read(fit_columns, 0, read_error);
    reads.fitted_columns = fit_columns;
    reads.fitted_rows = rows.size();
    if (!read_error.empty()) {
        resolved.error = "dataset: could not read '" + sampled.database + "." +
                         sampled.table + "' to key its entities and fit its categories: " +
                         read_error;
        return resolved;
    }
    if (resolved.contract.entity_keys) {
        std::vector<nlohmann::json> values;
        values.reserve(rows.size());
        for (const nlohmann::json& row : rows) {
            // A projected read carries a missing value as null.
            if (!row.is_object() || !row.contains(resolved.contract.entity_column) ||
                row.at(resolved.contract.entity_column).is_null()) {
                resolved.error = "dataset: a row is missing text entity column '" +
                                 resolved.contract.entity_column + "'";
                return resolved;
            }
            values.push_back(row.at(resolved.contract.entity_column));
        }
        EntityKeyMappingResult mapped = derive_entity_key_mapping(
            resolved.contract.entity_column, values);
        if (!mapped.ok()) {
            resolved.error = std::move(mapped.error);
            return resolved;
        }
        resolved.contract.entity_keys = std::move(mapped.mapping);
    }
    if (!resolved.contract.categorical_features.empty()) {
        // The training rows: with a time cutoff, only the rows the model may see.
        // A row whose time does not parse is not a training row either.
        std::vector<nlohmann::json> training;
        const std::vector<nlohmann::json>* fit_rows = &rows;
        if (!resolved.contract.time_column.empty()) {
            for (const nlohmann::json& row : rows) {
                if (!row.is_object()) continue;
                const auto time = row.find(resolved.contract.time_column);
                if (time == row.end()) continue;
                const std::optional<double> seconds = zetta::util::parse_time_seconds(*time);
                if (seconds && *seconds <= resolved.contract.as_of) training.push_back(row);
            }
            fit_rows = &training;
        }
        std::vector<CategoricalEncoding> kept;
        for (const CategoricalEncoding& sampled_encoding :
             resolved.contract.categorical_features) {
            const std::string prefix = categorical_note_prefix(sampled_encoding.column);
            auto note = std::ranges::find_if(resolved.notes, [&prefix](const std::string& line) {
                return line.starts_with(prefix);
            });
            CategoricalFit fit = fit_category(sampled_encoding.column, sampled.columns, *fit_rows);
            const std::string rows_counted =
                "training rows (" + std::to_string(fit_rows->size()) + ")";
            if (!fit.ok()) {
                const std::string reason = "not used: '" + sampled_encoding.column +
                                           "' (not numeric, and across every training "
                                           "row: " + fit.reason + ")";
                if (note != resolved.notes.end()) {
                    *note = reason;
                } else {
                    resolved.notes.push_back(reason);
                }
                continue;
            }
            const std::string fitted = categorical_note(*fit.encoding, rows_counted.c_str());
            if (note != resolved.notes.end()) {
                *note = fitted;
            } else {
                resolved.notes.push_back(fitted);
            }
            kept.push_back(std::move(*fit.encoding));
        }
        resolved.contract.categorical_features = std::move(kept);
        if (resolved.contract.feature_columns.empty() &&
            resolved.contract.categorical_features.empty()) {
            resolved.error =
                "dataset: no numeric or categorical feature columns beside the entity and "
                "label — a label with no features is not a training set. Add engineered "
                "numeric columns to the query, or name them with the dataset contract's "
                "feature_columns.";
            return resolved;
        }
    }
    return resolved;
}

nlohmann::json to_json(const DatasetContract& contract) {
    nlohmann::json value{{"entity_column", contract.entity_column},
                         {"label_column", contract.label_column},
                         {"time_column", contract.time_column},
                         {"as_of", contract.as_of},
                         {"feature_columns", contract.feature_columns}};
    if (contract.entity_keys) {
        value["entity_key_mapping"] = contract.entity_keys->to_json();
    }
    if (!contract.categorical_features.empty()) {
        value["categorical_features"] = to_json(contract.categorical_features);
    }
    return value;
}

std::optional<DatasetContract> dataset_contract_from_json(const nlohmann::json& value,
                                                          std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<DatasetContract> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("dataset_contract: not a JSON object");

    DatasetContract contract;
    for (const auto& [key, target] :
         {std::pair<const char*, std::string*>{"entity_column", &contract.entity_column},
          std::pair<const char*, std::string*>{"label_column", &contract.label_column},
          std::pair<const char*, std::string*>{"time_column", &contract.time_column}}) {
        const auto found = value.find(key);
        if (found == value.end()) continue;
        if (!found->is_string()) {
            return fail(std::string("dataset_contract.") + key + ": must be a string");
        }
        *target = found->get<std::string>();
    }
    if (const auto as_of = value.find("as_of"); as_of != value.end()) {
        const std::optional<double> seconds =
            zetta::util::parse_time_seconds(*as_of);
        if (!seconds) {
            return fail(
                "dataset_contract.as_of: must be a finite epoch number or "
                "ISO-8601 timestamp");
        }
        contract.as_of = *seconds;
    }
    if (const auto columns = value.find("feature_columns"); columns != value.end()) {
        if (!columns->is_array()) {
            return fail("dataset_contract.feature_columns: must be an array");
        }
        for (const nlohmann::json& column : *columns) {
            if (!column.is_string() || column.get<std::string>().empty()) {
                return fail("dataset_contract.feature_columns[]: must be non-empty strings");
            }
            contract.feature_columns.push_back(column.get<std::string>());
        }
    }
    if (const auto mapping = value.find("entity_key_mapping");
        mapping != value.end()) {
        std::string reason;
        contract.entity_keys = entity_key_mapping_from_json(*mapping, &reason);
        if (!contract.entity_keys) {
            return fail("dataset_contract.entity_key_mapping: " + reason);
        }
    }
    if (const auto categorical = value.find("categorical_features");
        categorical != value.end()) {
        std::string reason;
        contract.categorical_features = categorical_encodings_from_json(*categorical, &reason);
        if (!reason.empty()) {
            return fail("dataset_contract.categorical_features: " + reason);
        }
    }
    if (!contract.time_column.empty() && value.find("as_of") == value.end()) {
        return fail(
            "dataset_contract.as_of: required with a time_column — a cutoff column with no "
            "cutoff decides nothing, and inferring one would decide the leakage boundary "
            "silently");
    }
    return contract;
}

const nlohmann::json& dataset_contract_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["additionalProperties"] = false;
        document["description"] =
            "What a table's columns mean, for a dataset_path of the form database.table "
            "(#947). Every field is optional: what is omitted is inferred from the column "
            "names and a sample of the rows, and every inference is reported back.";
        document["properties"]["entity_column"] = {
            {"type", "string"},
            {"description",
             "the integer or text business key of the thing being scored, one row per entity"}};
        document["properties"]["label_column"] = {
            {"type", "string"}, {"description", "the {0,1} training target"}};
        document["properties"]["time_column"] = {
            {"type", "string"},
            {"description",
             "optional point-in-time cutoff column; requires as_of. Never inferred — it "
             "decides the leakage boundary"}};
        document["properties"]["as_of"] = {
            {"description",
             "the cutoff as a finite epoch number or ISO-8601 timestamp; only rows with "
             "time_column <= as_of contribute"},
            {"oneOf",
             nlohmann::json::array(
                 {{{"type", "number"}},
                  {{"type", "string"},
                   {"pattern",
                    "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"}}})}};
        document["properties"]["feature_columns"] = {
            {"type", "array"},
            {"items", {{"type", "string"}, {"minLength", 1}}},
            {"description",
             "the columns to train on; omit to use every numeric or boolean column that "
             "is not the entity, label, or time column, plus every text column with at "
             "most " + std::to_string(kCategoricalMaxLevels) +
                 " levels as a one-hot encoded category. A named text column is "
                 "one-hot encoded when it is a category"}};
        nlohmann::json mapping;
        mapping["type"] = "object";
        mapping["description"] =
            "resolved text business ids and their derived int64 keys";
        mapping["required"] = {"column", "entries"};
        mapping["additionalProperties"] = false;
        mapping["properties"]["column"] = {
            {"type", "string"}, {"minLength", 1}};
        mapping["properties"]["entries"]["type"] = "array";
        mapping["properties"]["entries"]["items"] = {
            {"type", "object"},
            {"required", {"derived_id", "original_value"}},
            {"additionalProperties", false},
            {"properties",
             {{"derived_id", {{"type", "integer"}}},
              {"original_value",
               {{"type", "string"}, {"minLength", 1}}}}}};
        document["properties"]["entity_key_mapping"] = std::move(mapping);
        document["properties"]["categorical_features"] = {
            {"type", "array"},
            {"items", categorical_encoding_schema()},
            {"description",
             "resolved low-cardinality text columns and their fitted levels (at most " +
                 std::to_string(kCategoricalMaxLevels) +
                 " each), one-hot encoded by a trainer that reads categorical columns. "
                 "Reported, and carried for distributed launches; a resolution refits it "
                 "from the rows rather than trusting a caller's copy"}};
        return document;
    }();
    return schema;
}

}  // namespace zetta::data
