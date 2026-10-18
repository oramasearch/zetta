#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

// **A dataset's columns, typed** (#1021): what `get_dataset` answers as
// `columns: [{name, type, nullable, null_fraction}]`.
//
// The engine stores rows as JSON objects, so a column's type is the JSON family
// its values share — the same vocabulary a JSON Lines partition body records
// (`partition_body.cpp`): `boolean`, `integer`, `number`, `string`, `object`,
// `array`, `null` (no value but null was seen) or `mixed` (values of several
// families; an integer column that meets a real number widens to `number`
// instead). A row that omits a column counts as a null in it.
//
// Two sources feed a profile: the rows themselves (a replicated table, whose rows
// every node holds) and, for a partition-only table whose rows live with their
// owners, the per-column stats each partition recorded in the catalog when it
// was written. A partition written before those stats existed reports its
// columns' types from the body's schema and no null share, and then the
// profile's `null_fraction` is unknown (absent) rather than guessed.
//
// The order of the profile's columns is not decided here: the caller passes the
// names in the order to show (see `zetta::data::order_columns`).
namespace zetta::data {

// One column of a dataset, as `get_dataset` reports it.
struct DatasetColumn {
    std::string name;
    std::string type = "null";       // the JSON family (see above)
    bool nullable = false;           // some row holds null for it, or omits it
    std::optional<double> null_fraction;  // nulls / rows; absent when unknown
};

// The JSON family of one value.
std::string_view json_type_family(const nlohmann::json& value);

// Two families combined: equal stays, `null` yields to the other, integer and
// number widen to `number`, anything else is `mixed`.
std::string merge_type_family(std::string_view left, std::string_view right);

// A Parquet physical type name (`int64`, `double`, `byte_array`, `boolean`, …)
// as the JSON family its values read back as.
std::string type_family_of_parquet(std::string_view parquet_type);

// Builds the typed column list from rows or from partition stats.
class ColumnProfile {
public:
    // One row of the dataset (a non-object row is counted and contributes no
    // column).
    void add_row(const nlohmann::json& row);

    // One partition's catalog stats and its row count: `column_stats`
    // (`[{name, type, nulls}]`, #1021) when the partition recorded it, otherwise
    // the body's `schema` (`[{name, type, optional}]`), which says nothing of how
    // many values are null.
    void add_partition_stats(const nlohmann::json& stats, std::uint64_t rows);

    // The typed columns for `names`, in that order. A name the profile never saw
    // is reported as an all-null column — or `unknown`, when partitions that
    // recorded no types at all were added.
    std::vector<DatasetColumn> columns(const std::vector<std::string>& names) const;

    // The stats a partition records beside its body (#1021): `[{name, type,
    // nulls}]` for `names`, in that order, over the rows added.
    nlohmann::json stats(const std::vector<std::string>& names) const;

private:
    struct Counts {
        std::string type = "null";
        std::uint64_t present = 0;   // rows that hold a non-null value
        bool typed = false;          // some source said what type it is
        bool nullable = false;       // a schema marked it optional
    };
    std::unordered_map<std::string, Counts> counts_;
    std::uint64_t rows_ = 0;
    std::uint64_t rows_without_counts_ = 0;  // from partitions with no null counts
    std::uint64_t rows_without_types_ = 0;   // from partitions with no types at all
};

}  // namespace zetta::data
