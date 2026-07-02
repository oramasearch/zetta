#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace zetta {

class Table;

// Result of importing records from a file.
struct ImportResult {
    std::size_t added = 0;
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// Append rows to `table` from a file. By extension:
//   .jsonl  — one JSON document per line (blank lines skipped)
//   .json (or anything else) — a JSON array (each element a row) or a single
//           JSON value (one row)
// Duplicates are allowed; nothing is de-duplicated. The file is fully parsed
// before any row is added, so a parse error imports nothing.
ImportResult import_records(Table& table, const std::filesystem::path& path);

}  // namespace zetta
