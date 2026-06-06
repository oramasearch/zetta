#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "temp_dir.hpp"
#include "zetta/bytes.hpp"
#include "zetta/database.hpp"
#include "zetta/import.hpp"
#include "zetta/table.hpp"

namespace fs = std::filesystem;
using zetta::Database;
using zetta::format_bytes;
using zetta::import_records;
using zetta::test::TempDir;

namespace {

fs::path write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

// A throwaway database + table to import into.
std::shared_ptr<zetta::Table> fresh_table(TempDir& tmp, const char* name) {
    static int counter = 0;
    Database db{"db", "d", tmp.path() / ("db" + std::to_string(counter++))};
    return db.create_table(name);  // table outlives db via shared_ptr
}

}  // namespace

TEST(ImportRecords, JsonArrayBecomesRows) {
    TempDir tmp;
    auto table = fresh_table(tmp, "t");
    const fs::path f = write_file(tmp.path() / "rows.json",
                                  R"([{"a":1},{"a":2},{"a":3}])");
    const auto r = import_records(*table, f);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.added, 3u);
    EXPECT_EQ(table->row_count(), 3u);
    EXPECT_EQ(table->store().at(1)["a"].get<int>(), 2);
}

TEST(ImportRecords, JsonSingleObjectBecomesOneRow) {
    TempDir tmp;
    auto table = fresh_table(tmp, "t");
    const fs::path f = write_file(tmp.path() / "one.json", R"({"k":"v"})");
    const auto r = import_records(*table, f);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.added, 1u);
}

TEST(ImportRecords, JsonlOnePerLine) {
    TempDir tmp;
    auto table = fresh_table(tmp, "t");
    const fs::path f = write_file(tmp.path() / "rows.jsonl",
                                  "{\"a\":1}\n{\"a\":2}\n\n{\"a\":3}\n");
    const auto r = import_records(*table, f);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.added, 3u);  // blank line skipped
}

TEST(ImportRecords, DuplicatesAreAllowed) {
    TempDir tmp;
    auto table = fresh_table(tmp, "t");
    const fs::path f = write_file(tmp.path() / "dups.jsonl",
                                  "{\"a\":1}\n{\"a\":1}\n{\"a\":1}\n");
    const auto r = import_records(*table, f);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.added, 3u);
    EXPECT_EQ(table->row_count(), 3u);
}

TEST(ImportRecords, InvalidJsonImportsNothing) {
    TempDir tmp;
    auto table = fresh_table(tmp, "t");
    const fs::path f = write_file(tmp.path() / "bad.jsonl",
                                  "{\"a\":1}\nnot json\n");
    const auto r = import_records(*table, f);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(table->row_count(), 0u);  // all-or-nothing
}

TEST(ImportRecords, MissingFileErrors) {
    TempDir tmp;
    auto table = fresh_table(tmp, "t");
    const auto r = import_records(*table, tmp.path() / "nope.json");
    EXPECT_FALSE(r.ok());
}

TEST(FormatBytes, HumanReadable) {
    EXPECT_EQ(format_bytes(0), "0 B");
    EXPECT_EQ(format_bytes(512), "512 B");
    EXPECT_EQ(format_bytes(1024), "1.0 KB");
    EXPECT_EQ(format_bytes(1536), "1.5 KB");
    EXPECT_EQ(format_bytes(1048576), "1.0 MB");
}
