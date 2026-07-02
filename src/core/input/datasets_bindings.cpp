#include "zetta/input/datasets_bindings.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/parquet/reader.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/data/table.hpp"

namespace zetta::input {
namespace {

// A short, identifiable label for a step (its name, else its id) — for error and
// lineage strings, mirroring the executor's own step_label.
std::string step_label(const ir::Node& step) {
    return step.name.empty() ? step.id : step.name;
}

// The bare verb a call lowered to: the `python.call` provenance, else the node's
// name (the lowering always sets both, but be tolerant of a hand-built node).
std::string load_verb(const ir::Node& step) {
    if (step.attributes.is_object()) {
        const auto python = step.attributes.find("python");
        if (python != step.attributes.end() && python->is_object()) {
            const auto call = python->find("call");
            if (call != python->end() && call->is_string()) {
                return call->get<std::string>();
            }
        }
    }
    return step.name;
}

// HuggingFace builder names: `load_dataset("parquet", data_files="…")` names the
// *builder*, not the path — so when the first positional is one of these the real
// source is a data_files / data_dir keyword.
bool is_builder_name(std::string_view value) {
    return value == "parquet" || value == "csv" || value == "json" ||
           value == "arrow" || value == "text";
}

// Derive a deterministic, filesystem-safe table name from a dataset source (+ an
// optional split): the basename after the last '/', minus a recognized file
// extension, lowercased with every run of non-alphanumeric characters folded to a
// single '_' and trimmed. Empty result falls back to "dataset".
std::string dataset_table_name(const std::string& source,
                               const std::string& split) {
    std::string base = source;
    if (const auto slash = base.find_last_of("/\\");
        slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    for (const std::string_view ext :
         {".parquet", ".pq", ".jsonl", ".json", ".csv", ".arrow"}) {
        if (base.size() > ext.size() &&
            std::string_view(base).substr(base.size() - ext.size()) == ext) {
            base = base.substr(0, base.size() - ext.size());
            break;
        }
    }
    if (!split.empty()) base += "_" + split;

    std::string name;
    name.reserve(base.size());
    bool pending_separator = false;
    for (const char character : base) {
        const char lower =
            static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
            if (pending_separator && !name.empty()) name.push_back('_');
            pending_separator = false;
            name.push_back(lower);
        } else {
            pending_separator = true;
        }
    }
    if (name.empty()) name = "dataset";
    return name;
}

// A free table name in `database` starting from `base`: `base`, then `base_2`,
// `base_3`, … so a second load of the same dataset does not collide.
std::string unique_table_name(const Database& database, std::string base) {
    if (!database.find_table(base)) return base;
    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (!database.find_table(candidate)) return candidate;
    }
}

// Apply the column projection in place: keep only the named columns on each row,
// in the requested order. Returns "" on success, else a reason naming the first
// requested column that is absent from the dataset (checked against the first row,
// since every row carries the same leaf-column key set).
std::string project_rows(std::vector<nlohmann::json>& rows,
                         const std::vector<std::string>& columns) {
    if (columns.empty() || rows.empty()) return "";

    std::set<std::string> available;
    if (rows.front().is_object()) {
        for (auto it = rows.front().begin(); it != rows.front().end(); ++it) {
            available.insert(it.key());
        }
    }
    for (const std::string& column : columns) {
        if (available.find(column) == available.end()) {
            return "requested column '" + column +
                   "' is not present in the dataset";
        }
    }
    for (nlohmann::json& row : rows) {
        if (!row.is_object()) continue;
        nlohmann::json projected = nlohmann::json::object();
        for (const std::string& column : columns) {
            const auto found = row.find(column);
            if (found != row.end()) projected[column] = *found;
        }
        row = std::move(projected);
    }
    return "";
}

}  // namespace

bool is_dataset_load_step(const ir::Node& step) {
    if (step.kind != ir::StepKind::Connect) return false;
    const std::string verb = load_verb(step);
    return verb == "load_dataset" || verb == "load" || verb == "read_parquet";
}

std::optional<DatasetLoadSpec> dataset_load_spec(const ir::Node& step) {
    if (!step.attributes.is_object()) return std::nullopt;
    const auto python = step.attributes.find("python");
    if (python == step.attributes.end() || !python->is_object()) {
        return std::nullopt;
    }

    // Positional string arguments, in order.
    std::vector<std::string> positionals;
    if (const auto args = python->find("args");
        args != python->end() && args->is_array()) {
        for (const nlohmann::json& argument : *args) {
            if (argument.is_string()) {
                positionals.push_back(argument.get<std::string>());
            }
        }
    }

    const nlohmann::json* kwargs = nullptr;
    if (const auto found = python->find("kwargs");
        found != python->end() && found->is_object()) {
        kwargs = &*found;
    }
    const auto kw_string = [&](const char* key) -> std::optional<std::string> {
        if (!kwargs) return std::nullopt;
        const auto it = kwargs->find(key);
        if (it != kwargs->end() && it->is_string()) {
            return it->get<std::string>();
        }
        return std::nullopt;
    };

    DatasetLoadSpec spec;

    // Source: the first positional, unless it names a builder + there is a
    // data_files / data_dir / path keyword (the HF builder-with-data shape).
    const bool first_is_builder =
        !positionals.empty() && is_builder_name(positionals.front());
    std::optional<std::string> keyword_source;
    for (const char* key : {"data_files", "data_dir", "path"}) {
        if (auto value = kw_string(key)) {
            keyword_source = std::move(value);
            break;
        }
    }
    if (!positionals.empty() && !first_is_builder) {
        spec.source = positionals.front();
    } else if (keyword_source) {
        spec.source = *keyword_source;
    } else if (!positionals.empty()) {
        spec.source = positionals.front();  // builder name, but no data_files given
    }
    if (spec.source.empty()) return std::nullopt;

    // Column projection: a `columns=` keyword — a list of strings (or a single one).
    if (kwargs) {
        const auto columns = kwargs->find("columns");
        if (columns != kwargs->end()) {
            if (columns->is_array()) {
                for (const nlohmann::json& column : *columns) {
                    if (column.is_string()) {
                        spec.columns.push_back(column.get<std::string>());
                    }
                }
            } else if (columns->is_string()) {
                spec.columns.push_back(columns->get<std::string>());
            }
        }
    }

    if (auto split = kw_string("split")) spec.split = *split;
    return spec;
}

DatasetResolver default_dataset_resolver() {
    return [](const std::string& source) -> DatasetResolveResult {
        DatasetResolveResult result;
        std::error_code error_code;
        const std::filesystem::path path(source);
        if (!std::filesystem::is_regular_file(path, error_code) || error_code) {
            result.error =
                "dataset source '" + source +
                "' is not a readable local file (a HuggingFace dataset name needs "
                "an injected resolver; this build has no network access)";
            return result;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            result.error = "cannot open dataset file '" + source + "'";
            return result;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        result.bytes = buffer.str();
        if (result.bytes.empty()) {
            result.error = "dataset file '" + source + "' is empty";
        }
        return result;
    };
}

DatasetLoadResult run_load_dataset_step(const ir::Node& step, Node& node,
                                        const DatasetResolver& resolver,
                                        const LoadDatasetOptions& options) {
    DatasetLoadResult result;

    const auto spec = dataset_load_spec(step);
    if (!spec) {
        result.error =
            "load step '" + step_label(step) + "' names no dataset source to load";
        return result;
    }

    // Resolve the dataset source to its Parquet bytes through the IO seam. (Guard a
    // caller that passed an empty std::function by falling back to the default.)
    const DatasetResolveResult bytes =
        resolver ? resolver(spec->source)
                 : default_dataset_resolver()(spec->source);
    if (!bytes.ok()) {
        result.error = bytes.error;
        return result;
    }

    // Decode with the native Parquet reader (no Arrow / parquet-cpp).
    parquet::ParquetRowsResult parsed = parquet::read_parquet_rows(bytes.bytes);
    if (!parsed.ok()) {
        result.error = "parquet '" + spec->source + "': " + parsed.error;
        return result;
    }

    // Honor the column projection: the table holds only the named columns.
    if (const std::string projection_error =
            project_rows(parsed.rows, spec->columns);
        !projection_error.empty()) {
        result.error = projection_error;
        return result;
    }

    // Materialize into a (partitioned) table on the node.
    std::shared_ptr<Database> database = node.find_database(options.database);
    if (!database) database = node.create_database(options.database);
    if (!database) {
        result.error = "could not create database '" + options.database + "'";
        return result;
    }
    result.database = database;

    const std::string table_name = unique_table_name(
        *database, dataset_table_name(spec->source, spec->split));
    std::shared_ptr<Table> table = database->create_table(table_name);
    if (!table) {
        result.error = "could not create table '" + table_name + "'";
        return result;
    }
    result.table = table;

    for (nlohmann::json& row : parsed.rows) table->add_row(std::move(row));
    result.rows = table->row_count();

    // When a partitioner is configured, write the table through the partitioned
    // write path so it is a *partitioned* table (blobs + catalog placement).
    if (options.partitioner) {
        table->set_partitioner(options.partitioner);
        PartitionWriteResult written =
            write_partitioned(node, *database, *table, options.members);
        if (!written.ok()) {
            result.error = "partition: " + written.error;
            return result;
        }
        result.partitions = std::move(written.partitions);
    }

    return result;
}

DatasetStepExecutor::DatasetStepExecutor(Node& node, DatasetResolver resolver,
                                         LoadDatasetOptions options)
    : node_(node),
      resolver_(std::move(resolver)),
      options_(std::move(options)) {}

StepOutcome DatasetStepExecutor::run_step(const ir::Node& step) const {
    // Only a Parquet dataset-load connect step is ours; everything else (other
    // step kinds, other connect verbs) runs through the default executor.
    if (!is_dataset_load_step(step)) return fallback_.run_step(step);

    const DatasetLoadResult loaded =
        run_load_dataset_step(step, node_, resolver_, options_);
    StepOutcome outcome;
    if (!loaded.ok()) {
        outcome.ok = false;
        outcome.error = loaded.error;
        return outcome;
    }

    // A connect step contributes a lineage source + a dataset output referencing
    // the real, populated table (a tbl: id, not a logical placeholder).
    if (const auto spec = dataset_load_spec(step)) {
        outcome.sources.push_back(spec->source);
    } else {
        outcome.sources.push_back(step_label(step));
    }
    if (loaded.table) {
        outcome.outputs.push_back(
            RunOutput{"dataset", loaded.table->name(), "tbl:" + loaded.table->id()});
    }
    return outcome;
}

}  // namespace zetta::input
