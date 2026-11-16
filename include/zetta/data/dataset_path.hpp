// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/categorical_encoding.hpp"

// **The one `dataset_path` grammar** (#947), and what a table's columns MEAN.
//
// Two things were both called a dataset and nothing converted one into the other:
//
//   * the **connector/ingest path** (`add_dataset`, `create_dataset_from_query`)
//     lands rows as parquet partitions placed across the cluster, addressed
//     `database.table` and surfaced in `system.datasets`;
//   * the **run path** wanted a filesystem directory holding a `dataset.json`
//     manifest plus its row files.
//
// So the data a customer actually brings — through a connection, through a query —
// could not reach the thing that trains a model on it. `dataset_path` accepted only
// the second form, and `dataset_path = "churn_demo.accounts"` failed with `cannot
// open dataset.json` while the rows sat there, partitioned and healthy.
//
// This module is the bridge. It parses BOTH forms of `dataset_path` and, for a
// table, answers the question a directory answers in its manifest and a partitioned
// table does not: **which column is the entity, which is the label, and which are
// the features.**
//
// **Why inference, and why it is conservative.** A warehouse query returns whatever
// columns the analyst selected; requiring the caller to spell the contract out every
// time would make the plain-language path ("predict which accounts will churn")
// useless, because the agent does not know which column holds the label. So the
// contract is INFERRED from the column names, the column types the partition catalog
// records, and a sample of the rows where the types do not decide (#1203), by the ranked
// rules below — and every inference is reported back in `notes`, so a caller reads
// what was assumed rather than discovering it in a model. Anything inferred can be
// overridden explicitly, and an inference that cannot be made **fails with the list
// of names it looked for** instead of guessing.
//
// **The one thing that is never inferred is the time cutoff.** A `time_column` +
// `as_of` decide the leakage boundary — which rows a feature may see. Guessing that
// would silently decide whether the model is honest, so a time dimension applies
// only when the caller names it. With no time column the rows are taken as already
// point-in-time correct (the warehouse query did the windowing), which is the shape
// a connector query actually produces.
//
// **No new JSON format.** The contract is a value type with a hand-written
// non-throwing `from_json` mirroring `dataset_contract_schema()`, the boundary rule
// for value-type DTOs. Nothing here throws.
//
// #947 · #948 · docs/data/dataset-path.md.
namespace zetta {
class Node;
}
namespace zetta::cluster {
class Cluster;
}

namespace zetta::data {

struct DatasetContract;

// A text business identifier's deterministic numeric representation inside the
// numeric ML engine. The map is dataset-scoped: two different strings may never
// claim the same derived key in one dataset. Outputs use original() so the hash is
// never exposed as the entity a customer must join against.
struct EntityKeyMapping {
    std::string column;
    std::map<std::int64_t, std::string> derived_to_original;
    std::map<std::string, std::int64_t> original_to_derived;

    bool empty() const noexcept {
        return derived_to_original.empty();
    }
    std::optional<std::int64_t> derived(std::string_view original) const;
    nlohmann::json original(std::int64_t derived) const;
    nlohmann::json to_json() const;
};

using EntityKeyHash = std::function<std::uint64_t(std::string_view)>;

struct EntityKeyMappingResult {
    std::shared_ptr<const EntityKeyMapping> mapping;
    std::string error;

    bool ok() const noexcept {
        return error.empty();
    }
};

// Build a collision-checked map for one entity column. `hash` is injectable only
// so the collision refusal can be proved; production uses a stable FNV-1a hash.
EntityKeyMappingResult derive_entity_key_mapping(
    std::string column, const std::vector<nlohmann::json>& values,
    const EntityKeyHash& hash = {});
// A read answer's view of an entity-key mapping (#1202): `{column,
// entry_count}` in place of `{column, entries}`. The entries list every distinct
// text id of the dataset, so a Run or Model read carrying them grows with the
// row count (222 KB for get_run on 1,500 rows). Reads leave them out unless
// the caller asks for them; the stored record is unchanged.
nlohmann::json entity_key_mapping_summary(const nlohmann::json& mapping);

// Replace every `entity_key_mapping` object with entries, or referencing its
// entity-key table (#1255), anywhere in `document` by its summary. Returns how
// many it replaced.
std::size_t summarize_entity_key_mappings(nlohmann::json& document);

// The JSON Schema of entity_key_mapping_summary()'s answer.
const nlohmann::json& entity_key_mapping_summary_schema();

std::shared_ptr<const EntityKeyMapping> entity_key_mapping_from_json(
    const nlohmann::json& value, std::string* error = nullptr);

// Preserve integer entities and restore text entities for every user-facing row.
nlohmann::json entity_output(const DatasetContract& contract,
                             std::int64_t derived);

// Which of the two forms a `dataset_path` used.
enum class DatasetPathKind {
    Directory,  // a filesystem directory holding dataset.json + its row files
    Table,      // `database.table` (or a bare `table`) in this cluster's catalog
};

std::string_view to_string(DatasetPathKind kind);

// A parsed `dataset_path`. Exactly one of the two shapes is populated.
struct DatasetPath {
    DatasetPathKind kind = DatasetPathKind::Directory;
    std::filesystem::path directory;  // Directory
    std::string database;             // Table
    std::string table;                // Table
};

// Parse `path` under the one grammar:
//
//   * anything containing a `/` or `\`, ending in a path separator, or starting
//     with `.` or `~` → **Directory**;
//   * `database.table` → **Table**;
//   * a bare `table` (no dot) → **Table** in `default_database`.
//
// The separator test comes first because a path is the unambiguous form: a
// directory name may legitimately contain a dot (`./data/churn.v2`), while a table
// name may not contain a slash. `~` is NOT expanded here — expansion belongs to the
// caller that owns the filesystem, not to a grammar.
//
// Pure, non-throwing, and never touches the filesystem (#1433). A Directory is only
// a parse: the caller that opens it resolves it under the node's import root with
// `resolve_import_path` (`zetta/data/import_path.hpp`), which refuses an absolute
// path, `..`, `~` and symbolic links.
DatasetPath parse_dataset_path(std::string_view path,
                               const std::string& default_database = "datasets");

// What a table's columns mean — the contract a dataset directory carries in its
// manifest. Every field may be supplied by the caller; the ones left empty are
// inferred.
struct DatasetContract {
    // The entity key: an integer passes through unchanged; text receives a
    // dataset-scoped derived key internally and remains text in every output.
    std::string entity_column;

    // The numeric training target. Inference selects binary labels; an explicit
    // override also supports continuous numeric targets. Never a feature.
    std::string label_column;

    // Optional time column containing finite epoch numbers or ISO-8601 strings.
    // When set, rows are restricted to `time <= as_of` before anything is
    // computed — the leakage boundary. `as_of` is normalized to epoch seconds.
    // Empty means the rows are taken as already point-in-time correct.
    std::string time_column;
    double as_of = 0.0;

    // The numeric or boolean columns carried through as X, in a stable order.
    // Never contains the entity, label, or time column.
    std::vector<std::string> feature_columns;

    // Low-cardinality text columns, one-hot encoded (#1203). Kept apart from
    // `feature_columns` because only a trainer that reads them (the M-1 GBDT's
    // table path) may use them; every other methodology reads numeric columns and
    // ignores this list. Inference fits the levels on the sampled rows and
    // `resolve_dataset` refits them on every training row. See
    // categorical_encoding.hpp for the rules.
    std::vector<CategoricalEncoding> categorical_features;

    // Populated only for a text entity column. Serialized with the contract for
    // distributed launches. Runs and Models persist it as a reference to their
    // rows of the entity-key table (#1255).
    std::shared_ptr<const EntityKeyMapping> entity_keys;

    bool operator==(const DatasetContract& other) const;
};

// The result of inferring a contract: what was decided, why, and — when it could
// not be decided — the reason, naming what was looked for.
struct DatasetContractResult {
    DatasetContract contract;
    // One line per inference actually made ("label column: 'churned' (binary in the
    // sample)"). A caller shows these; a silent inference is the failure mode this
    // whole module exists to avoid.
    std::vector<std::string> notes;
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// The column names inference recognizes, in the order it prefers them. Exposed so
// the failure message, the docs, and the tests all read from one list rather than
// three copies that drift.
const std::vector<std::string>& entity_column_candidates();
const std::vector<std::string>& label_column_candidates();

class DatasetPartition;

// What the partition catalog says about one column, across every partition of a
// table (#1203 item 3). Each partition body records its own schema in the catalog
// (#837): the Parquet physical type of each column, or the JSON family for a JSON
// Lines body. Merged over the partitions, that answers most of what inference asks
// of a column without reading a row:
//
//   * `Boolean`   — every value is true/false: numeric, binary and integer.
//   * `Integer`   — every value is a whole number (a boolean partition widens to
//                   it): numeric and integer; whether it is 0/1 is not recorded.
//   * `Number`    — numbers, some real: numeric; integer-ness is not recorded.
//   * `Null`      — no partition holds a value (a JSON Lines body records `null`).
//   * `Undecided` — text, a column mixing families, or a Parquet `byte_array`,
//                   which is also what an all-null Parquet column is written as.
//                   Only values can decide it.
//
// An integer partition and a real-number partition merge to `Number`; anything
// merged with `Undecided` is `Undecided`; `Null` merges to the other side.
enum class CatalogColumnType { Boolean, Integer, Number, Null, Undecided };

std::string_view to_string(CatalogColumnType type);

// The catalog's column types for one table, or why the catalog cannot type it.
// A table can only be typed from the catalog when EVERY row is in a partition
// that recorded its schema: a table that was never partitioned, a partition
// written before #837 recorded schemas, or a catalog that covers fewer rows than
// the table holds (rows inserted after the ingest) all leave `unavailable` set,
// and the contract is typed from sampled rows as before.
struct CatalogColumnTypes {
    std::map<std::string, CatalogColumnType> columns;
    std::size_t partitions = 0;
    std::string unavailable;  // empty when the catalog types the table

    bool available() const { return unavailable.empty(); }
    // The column's type; `Undecided` for a column the catalog does not record.
    CatalogColumnType type_of(const std::string& column) const;
};

// Merge the `stats.schema` of `table_id`'s entries in `partitions`. `rows` is the
// table's row count; the catalog must account for all of them.
CatalogColumnTypes catalog_column_types(
    const std::vector<std::shared_ptr<DatasetPartition>>& partitions,
    const std::string& table_id, std::uint64_t rows);

// The columns whose values inference must still read when the catalog types the
// rest, given the caller's `overrides`:
//
//   * every `Undecided` column — text has to be looked at to tell a category
//     (2 to 32 levels, not an identifier, not a date) from anything else, and a
//     text entity has to be checked for empty values;
//   * a label candidate (by name, when no label is named) typed `Integer` or
//     `Number` — the catalog does not record whether the values are 0/1;
//   * an entity candidate, or the named entity, typed `Number` — the catalog does
//     not record whether the reals are whole.
//
// Everything else is decided by its catalog type. In catalog order.
std::vector<std::string> columns_needing_values(const std::vector<std::string>& columns,
                                                const CatalogColumnTypes& catalog,
                                                const DatasetContract& overrides);

// Infer the parts of `overrides` that were left empty, from `columns` (the table's
// column names, in catalog order) and `sample` (rows read from the table — enough
// to judge a column's type; an empty sample means only names can be used).
//
// With `catalog` (available), a column's type is taken from the partition catalog
// wherever the catalog decides it, and `sample` is consulted only for the columns
// columns_needing_values() names — it may carry just those. Every decision is the
// one the sample would have made, with one deliberate difference: a column the
// catalog types numeric but whose sampled rows are all null is a feature (the
// catalog has seen its values; the sample had not), where the sample alone left it
// out with "no values in the sampled rows".
//
// The rules, in order:
//
//   * **label** — the first `label_column_candidates()` name present whose sampled
//     values are all binary ({0,1}, true/false, or empty). Binary-ness matters more
//     than the name: a column called `label` holding free text is not a target.
//   * **entity** — the first `entity_column_candidates()` name present whose
//     sampled values are consistently integer or non-empty text. Text values receive
//     collision-checked derived int64 keys for compute and are restored in outputs.
//   * **features** — every remaining column whose sampled values are all numeric
//     or boolean, in catalog order, excluding the entity, label, and time
//     columns. A remaining text column that fits categorical_encoding.hpp's rules
//     (at most `kCategoricalMaxLevels` levels, not an identifier, not a date) goes
//     to `categorical_features`; any other column is left out with a note saying
//     why. At least one numeric or categorical feature is required: a label with
//     no features is not a training set. A text column the caller names in
//     `feature_columns` stays there (a text methodology reads it as it is) and is
//     ALSO recorded in `categorical_features` when it is a category; when it is
//     not, a note says why it is not one-hot encoded.
//   * **time / as_of** — never inferred (see the header note). Passed through from
//     `overrides` untouched.
//
// A column absent from `columns` but named in `overrides` is an error — a caller
// naming a column that is not there has a typo, not an intent.
DatasetContractResult infer_dataset_contract(const std::vector<std::string>& columns,
                                             const std::vector<nlohmann::json>& sample,
                                             const DatasetContract& overrides = {},
                                             const CatalogColumnTypes* catalog = nullptr);

// Read up to `limit` rows of `database`.`table`, for typing the columns. A zero
// limit means all rows; entity mapping uses it to detect collisions dataset-wide.
//
// An injection point (not a hard dependency) so the inference above is unit-testable
// against rows built in a test, and so the read path stays where it belongs: the
// default reads through SQL, which routes a partition-only table through the
// scatter-gather select and so fails loudly on a degraded catalog rather than typing
// columns off a fraction of the data.
using DatasetSampler = std::function<std::vector<nlohmann::json>(
    const std::string& database, const std::string& table, std::size_t limit,
    std::string& error)>;

// The default sampler: `SELECT * FROM <database>.<table> LIMIT <limit>` (no LIMIT
// for a zero limit) through the node's own query service. Pass the node's
// `Cluster` so a partition-only table is read through the scatter-gather select —
// without it the read is node-local and would type the columns off whatever slice
// happens to live here.
//
// **The cost of the LIMIT is the query service's**: until the SQL `LIMIT` is pushed
// into the scan (#1021) it cuts the result, not the scan, so the whole table is
// still read; with the pushdown only the first `limit` rows are. The rows kept are
// the same either way. resolve_dataset() no longer uses it when no
// sampler is injected — it types the contract from the partition catalog and reads
// only the columns that still need values (read_dataset_columns below). The
// methodologies that read every row for training still call it with a zero limit.
DatasetSampler default_dataset_sampler(Node& node,
                                       const zetta::cluster::Cluster* cluster = nullptr);

// The same read, executed AS a caller (#1396): the query service carries the
// caller's identity, so the SELECT is authorized like the caller's own SQL — a table
// it holds no `sql:read` on is refused, a column it may not read comes back null,
// and row security keeps only the rows its policies admit. An empty `acting_key` is
// the trusted local session, exactly default_dataset_sampler.
DatasetSampler caller_dataset_sampler(Node& node, const zetta::cluster::Cluster* cluster,
                                      std::string acting_key, bool acting_is_user);

// Read `columns` of up to `limit` rows (zero: every row) of `database`.`table`
// through the node's query service: `SELECT <columns> FROM <database>.<table>
// LIMIT <limit>`. Rows carry only the named columns. A column whose name the
// SQL subset cannot spell as a bare identifier (it has no quoted identifiers), or a
// projected query the executor refuses, falls back to `SELECT *` and projects the
// rows here, so the answer is the same either way.
//
// Like default_dataset_sampler, the LIMIT bounds the scan once the query service
// pushes it down (#1021); before that it cuts the result of a whole-table scan.
std::vector<nlohmann::json> read_dataset_columns(Node& node,
                                                 const zetta::cluster::Cluster* cluster,
                                                 const std::string& database,
                                                 const std::string& table,
                                                 const std::vector<std::string>& columns,
                                                 std::size_t limit, std::string& error);

// How many rows the default resolution samples. Enough to catch a column that is
// integer in its first rows and fractional later, small enough to be free.
inline constexpr std::size_t kDefaultSampleRows = 256;

// A table resolved against the live dataset catalog and sampled through the
// canonical query path. This is the shared read primitive behind contract
// inference and dataset analysis: catalog lookup, degraded-data refusal, and
// distributed reads belong in one place.
struct SampledDataset {
    std::string table_id;
    std::string database;
    std::string table;
    std::uint64_t rows = 0;
    std::vector<std::string> columns;
    std::vector<nlohmann::json> sample;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Resolve and sample a Table-form path. Directory paths, missing/empty tables,
// degraded partition catalogs, and read failures are returned as path-qualified
// errors. Passing `cluster` keeps partition-only reads on scatter/gather.
SampledDataset sample_dataset(Node& node, const DatasetPath& path,
                              const DatasetSampler& sampler = {},
                              std::size_t sample_rows = kDefaultSampleRows,
                              const zetta::cluster::Cluster* cluster = nullptr);

// What resolve_dataset() read to type the contract, so a caller (and a test) can
// see exactly which rows were touched rather than assume none or all were.
struct DatasetReads {
    // The contract was typed from the partition catalog. When false,
    // `catalog_unavailable` says why and the first `sampled_rows` rows were read
    // whole (`SELECT *`), as before #1203 item 3.
    bool typed_from_catalog = false;
    std::string catalog_unavailable;
    std::size_t catalog_partitions = 0;

    // The typing read: the columns whose values inference looked at, from the
    // first `sampled_rows` rows. Empty with typed_from_catalog: no rows were read
    // to type the contract.
    std::vector<std::string> sampled_columns;
    std::size_t sampled_rows = 0;

    // The fitting read, over every row: a text entity's keys, each category's
    // levels (refitted on the training rows), and the time column that picks the
    // training rows. Empty when there is no text entity and no category.
    std::vector<std::string> fitted_columns;
    std::size_t fitted_rows = 0;
};

// A `dataset_path` resolved against the live catalog: the table it names, and what
// its columns mean.
struct ResolvedDataset {
    std::string table_id;   // the `Table::id()` the partition catalog records — what
                            //   the near-data runners take
    std::string database;
    std::string table;
    std::uint64_t rows = 0;

    DatasetContract contract;
    std::vector<std::string> notes;  // the inferences made, for the caller to show
    DatasetReads reads;              // what was read to make them
    std::string error;               // empty on success

    bool ok() const { return error.empty(); }
};

// Resolve a Table-form `dataset_path` against `node`: find the dataset in the live
// catalog (`Node::datasets()`), type its columns, and infer/validate the contract.
//
// **Typing (#1203 item 3).** The column types come from the partition catalog
// (catalog_column_types) when it covers the table. Rows are then read only for
// what the catalog cannot decide — columns_needing_values(): text columns (is it a
// category, an identifier, a date?), and a label or entity candidate whose 0/1-ness
// or whole-ness the catalog does not record. Those columns alone are read, from the
// first `sample_rows` rows. A table the catalog cannot type is sampled whole as
// before, and a note says why. With an injected `sampler` the same columns are
// needed, and the sampler's rows are used for them.
//
// **Fitting.** A text entity's keys and each category's levels are then fitted
// over every row (the levels over the training rows, those at or before `as_of`):
// that read carries the entity, the categories and the time column only. A table
// with no text entity and no category reads no row at all past the typing read.
//
// Fails, non-throwing and with a path-qualified reason, when: the path is the
// Directory form (the caller loads those itself — this says so rather than
// pretending); no such database/table exists (naming the near-misses it did find);
// the dataset is **degraded**, meaning this node cannot see all of its partitions
// and would type — and later train on — a fraction of the data; or the contract
// cannot be inferred.
ResolvedDataset resolve_dataset(Node& node, const DatasetPath& path,
                                const DatasetContract& overrides = {},
                                const DatasetSampler& sampler = {},
                                std::size_t sample_rows = kDefaultSampleRows,
                                const zetta::cluster::Cluster* cluster = nullptr);

nlohmann::json to_json(const DatasetContract& contract);
std::optional<DatasetContract> dataset_contract_from_json(const nlohmann::json& value,
                                                          std::string* error = nullptr);
const nlohmann::json& dataset_contract_schema();

}  // namespace zetta::data
