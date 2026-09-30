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
// contract is INFERRED from the column names and a sample of the rows, by the ranked
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
//   * anything containing a `/` or `\`, ending in a path separator, starting with
//     `.` or `~`, or naming an existing filesystem entry → **Directory**;
//   * `database.table` → **Table**;
//   * a bare `table` (no dot) → **Table** in `default_database`.
//
// The separator test comes first because a path is the unambiguous form: a
// directory name may legitimately contain a dot (`./data/churn.v2`), while a table
// name may not contain a slash. `~` is NOT expanded here — expansion belongs to the
// caller that owns the filesystem, not to a grammar.
//
// Pure, non-throwing, and does not touch the filesystem except for the
// existence test, which only ever moves a path INTO the Directory form.
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

    // Populated only for a text entity column. Serialized with the contract for
    // distributed launches, and persisted with Runs and Models.
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

// Infer the parts of `overrides` that were left empty, from `columns` (the table's
// column names, in catalog order) and `sample` (rows read from the table — enough
// to judge a column's type; an empty sample means only names can be used).
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
//     columns. At least one is required: a label with no features is not a
//     training set.
//   * **time / as_of** — never inferred (see the header note). Passed through from
//     `overrides` untouched.
//
// A column absent from `columns` but named in `overrides` is an error — a caller
// naming a column that is not there has a typo, not an intent.
DatasetContractResult infer_dataset_contract(const std::vector<std::string>& columns,
                                             const std::vector<nlohmann::json>& sample,
                                             const DatasetContract& overrides = {});

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

// The default sampler: `SELECT * FROM <database>.<table>` through the node's own
// query service, truncated to `limit` rows. Pass the node's `Cluster` so a
// partition-only table is read through the scatter-gather select — without it the
// read is node-local and would type the columns off whatever slice happens to live
// here.
//
// **It reads the whole table and keeps the first `limit` rows**: the SQL subset's
// LIMIT cuts the result, not the scan. That is bounded work on a demo-sized table and wasteful
// on a large one; the partition catalog already records each body's column types
// (#837), so typing the columns from the catalog instead — reading no rows at all —
// is the follow-up. Recorded rather than left as a surprise.
DatasetSampler default_dataset_sampler(Node& node,
                                       const zetta::cluster::Cluster* cluster = nullptr);

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
    std::string error;               // empty on success

    bool ok() const { return error.empty(); }
};

// Resolve a Table-form `dataset_path` against `node`: find the dataset in the live
// catalog (`Node::datasets()`), sample it, and infer/validate the contract.
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
