#include "zetta/data/dataset_path.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "zetta/data/dataset.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"

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
    if (value.is_string()) {
        const std::string text = lowered(value.get<std::string>());
        return text == "0" || text == "1" || text == "true" || text == "false" ||
               text == "t" || text == "f" || text == "yes" || text == "no";
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
                          " matched by name but is not binary in the sampled rows)";
            }
            return fail(std::move(reason));
        }
    }

    // --- the entity ---------------------------------------------------------
    // Must be integer-valued: entities are keyed by int64 the whole way through the
    // ML path, and a ranked list keyed by a number that joins back to nothing is
    // worse than a clear failure here.
    if (result.contract.entity_column.empty()) {
        std::vector<std::string> named_but_not_integer;
        for (const std::string& candidate : entity_column_candidates()) {
            const std::string column = find_column(columns, candidate);
            if (column.empty() || column == result.contract.label_column) continue;
            if (all_values(sample, column, is_integer_value)) {
                result.contract.entity_column = column;
                result.notes.push_back("entity column: '" + column +
                                       "' (integer across the sampled rows)");
                break;
            }
            named_but_not_integer.push_back(column);
        }
        if (result.contract.entity_column.empty()) {
            std::string reason = "dataset: no integer entity column found — looked for " +
                                 join(entity_column_candidates(), " / ") + ".";
            if (!named_but_not_integer.empty()) {
                reason += " " + join(named_but_not_integer, ", ") +
                          " matched by name but holds non-integer values; entities are keyed "
                          "by a 64-bit integer, so add an integer key column (or name one "
                          "with the dataset contract's entity_column).";
            } else {
                reason += " Name one with the dataset contract's entity_column.";
            }
            return fail(std::move(reason));
        }
    } else if (!sample.empty() &&
               !all_values(sample, result.contract.entity_column, is_integer_value)) {
        return fail("dataset: the entity column '" + result.contract.entity_column +
                    "' holds non-integer values; entities are keyed by a 64-bit integer");
    }

    // --- the features -------------------------------------------------------
    if (result.contract.feature_columns.empty()) {
        for (const std::string& column : columns) {
            if (column == result.contract.entity_column) continue;
            if (column == result.contract.label_column) continue;
            if (column == result.contract.time_column) continue;
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
        if (limit > 0 && rows.size() > limit) rows.resize(limit);
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
    resolved.notes = std::move(contract.notes);
    return resolved;
}

nlohmann::json to_json(const DatasetContract& contract) {
    return nlohmann::json{{"entity_column", contract.entity_column},
                          {"label_column", contract.label_column},
                          {"time_column", contract.time_column},
                          {"as_of", contract.as_of},
                          {"feature_columns", contract.feature_columns}};
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
        if (!as_of->is_number()) return fail("dataset_contract.as_of: must be a number");
        contract.as_of = as_of->get<double>();
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
             "the integer key of the thing being scored, one row per entity"}};
        document["properties"]["label_column"] = {
            {"type", "string"}, {"description", "the {0,1} training target"}};
        document["properties"]["time_column"] = {
            {"type", "string"},
            {"description",
             "optional point-in-time cutoff column; requires as_of. Never inferred — it "
             "decides the leakage boundary"}};
        document["properties"]["as_of"] = {
            {"type", "number"},
            {"description", "the cutoff: only rows with time_column <= as_of contribute"}};
        document["properties"]["feature_columns"] = {
            {"type", "array"},
            {"items", {{"type", "string"}, {"minLength", 1}}},
            {"description",
             "the numeric columns to train on; omit to use every numeric column that is "
             "not the entity, label, or time column"}};
        return document;
    }();
    return schema;
}

}  // namespace zetta::data
