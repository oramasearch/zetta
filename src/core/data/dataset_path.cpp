#include "zetta/data/dataset_path.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include "zetta/data/dataset.hpp"
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

std::string join(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) out += separator;
        out += items[index];
    }
    return out;
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
           feature_columns == other.feature_columns && mappings_equal;
}

std::string_view to_string(DatasetPathKind kind) {
    switch (kind) {
        case DatasetPathKind::Directory: return "directory";
        case DatasetPathKind::Table: return "table";
    }
    return "directory";
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

    // An existing filesystem entry wins over the table reading — this only ever
    // moves a path INTO the Directory form, so it can never steal a live table.
    std::error_code ignored;
    if (!path.empty() && std::filesystem::exists(std::filesystem::path(path), ignored)) {
        parsed.kind = DatasetPathKind::Directory;
        parsed.directory = std::filesystem::path(path);
        return parsed;
    }

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
                                             const DatasetContract& overrides) {
    DatasetContractResult result;
    result.contract = overrides;

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
            if (all_values(sample, column, is_binary_value)) {
                result.contract.label_column = column;
                result.notes.push_back("label column: '" + column +
                                       "' (binary across the sampled rows)");
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
    } else if (!sample.empty() &&
               !all_values(sample, result.contract.label_column,
                           is_numeric_value)) {
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
            if (all_values(sample, column, is_integer_value)) {
                result.contract.entity_column = column;
                result.notes.push_back("entity column: '" + column +
                                       "' (integer across the sampled rows)");
                break;
            }
            if (all_values(sample, column, is_text_value)) {
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
    } else if (!sample.empty()) {
        if (all_values(sample, result.contract.entity_column, is_integer_value)) {
            result.notes.push_back("entity column: '" +
                                   result.contract.entity_column +
                                   "' (integer across the sampled rows)");
        } else if (all_values(sample, result.contract.entity_column,
                              is_text_value)) {
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
            if (!all_values(sample, column, is_numeric_value)) {
                // Said out loud: a column left out silently is one the caller assumes
                // the model used (lake's UX review 2026-09-27; categorical features are
                // #957).
                result.notes.push_back("not used: '" + column +
                                       "' (not numeric; categorical columns are not "
                                       "supported yet)");
                continue;
            }
            result.contract.feature_columns.push_back(column);
        }
        if (result.contract.feature_columns.empty()) {
            return fail(
                "dataset: no numeric feature columns beside the entity and label — a label "
                "with no features is not a training set. Add engineered numeric columns to "
                "the query, or name them with the dataset contract's feature_columns.");
        }
        result.notes.push_back(
            "feature columns (" + std::to_string(result.contract.feature_columns.size()) +
            "): " + join(result.contract.feature_columns, ", "));
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
    return [&node, cluster](const std::string& database, const std::string& table,
                            std::size_t limit, std::string& error) {
        zetta::query::QueryService service(node, cluster);
        const zetta::query::QueryService::Outcome outcome =
            service.run("SELECT * FROM " + database + "." + table, database);
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

SampledDataset sample_dataset(Node& node, const DatasetPath& path,
                              const DatasetSampler& sampler,
                              std::size_t sample_rows,
                              const zetta::cluster::Cluster* cluster) {
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

    std::string sample_error;
    const DatasetSampler read = sampler ? sampler : default_dataset_sampler(node, cluster);
    sampled.sample =
        read(found->database, found->name, sample_rows, sample_error);
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
    const SampledDataset sampled =
        sample_dataset(node, path, sampler, sample_rows, cluster);
    if (!sampled.ok()) {
        resolved.error = sampled.error;
        return resolved;
    }

    resolved.table_id = sampled.table_id;
    resolved.database = sampled.database;
    resolved.table = sampled.table;
    resolved.rows = sampled.rows;

    DatasetContractResult contract =
        infer_dataset_contract(sampled.columns, sampled.sample, overrides);
    if (!contract.ok()) {
        resolved.error = contract.error;
        return resolved;
    }
    resolved.contract = std::move(contract.contract);
    if (resolved.contract.entity_keys) {
        std::string mapping_error;
        const DatasetSampler read =
            sampler ? sampler : default_dataset_sampler(node, cluster);
        const std::vector<nlohmann::json> rows =
            read(sampled.database, sampled.table, 0, mapping_error);
        if (!mapping_error.empty()) {
            resolved.error = "dataset: could not map text entity column '" +
                             resolved.contract.entity_column + "': " +
                             mapping_error;
            return resolved;
        }
        std::vector<nlohmann::json> values;
        values.reserve(rows.size());
        for (const nlohmann::json& row : rows) {
            if (!row.is_object() ||
                !row.contains(resolved.contract.entity_column)) {
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
    resolved.notes = std::move(contract.notes);
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
             "the numeric or boolean columns to train on; omit to use every numeric or "
             "boolean column that is not the entity, label, or time column"}};
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
        return document;
    }();
    return schema;
}

}  // namespace zetta::data
