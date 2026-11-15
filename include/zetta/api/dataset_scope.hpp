#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The **per-call dataset scope** (#1515, the zetta side of
// https://github.com/oramasearch/lake/issues/1055).
//
// A caller may confine one call to named datasets with the request header
// `Zetta-Dataset-Scope: <database>.<table>[, ...]` (`<database>/<table>` is the
// same; lake writes its work areas that way). The scope only ever narrows what
// the bearer may do, so it needs no trust of its own: whoever holds the bearer
// could make the unconfined call anyway. An empty value confines the call to no
// dataset at all.
//
// Under a scope:
//
//   * Only registry capability routes (`/v1/<capability>`) answer. `/mcp`, the
//     object byte routes and every other gated path answer 403, because none of
//     them says what it reads.
//   * A capability runs only when it declares what it reads for its input
//     (`Capability::dataset_reads`) and every dataset it reads is in the scope.
//     A capability that declares nothing is refused: fail closed.
//   * A refusal is 403 `unauthorized`, audited, and its `details` names
//     `datasets_read` and `outside`, so the caller learns what the call reads.
//
// Lake sends the header with an agent launch's work area on every engine call
// the launch credential makes, so the engine, not a second parser in lake,
// decides what the call may read. docs/governance/dataset-scope.md.
namespace zetta::api {

// The request header, lowercase as HTTP field lookups compare it.
inline constexpr std::string_view kDatasetScopeHeader = "zetta-dataset-scope";

// One dataset: a table in a database.
struct DatasetName {
    std::string database;
    std::string table;

    bool operator==(const DatasetName&) const = default;
    // `<database>.<table>`.
    std::string text() const { return database + "." + table; }
};

class DatasetScope {
public:
    // Confined to no dataset.
    DatasetScope() = default;

    // Parse a header value: entries separated by commas and/or whitespace, each
    // `<database>.<table>` or `<database>/<table>`, both parts non-empty and made
    // of letters, digits, `_` and `-`. Returns nullopt (and fills `error`) on the
    // first malformed entry: a scope that cannot be read never widens a call.
    static std::optional<DatasetScope> parse(std::string_view value,
                                             std::string* error = nullptr);

    bool contains(const DatasetName& dataset) const;
    const std::vector<DatasetName>& datasets() const { return datasets_; }

    // For a refusal: the entries as `<database>.<table>`, comma separated, or
    // "no dataset".
    std::string describe() const;

private:
    std::vector<DatasetName> datasets_;
};

}  // namespace zetta::api
