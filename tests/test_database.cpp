#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/database.hpp"

namespace fs = std::filesystem;
using zetta::Database;
using zetta::test::TempDir;

TEST(Database, CreateFindDropTable) {
    TempDir tmp;
    Database db{"db-id", "shop", tmp.path()};

    auto users = db.create_table("users");
    ASSERT_TRUE(users);
    EXPECT_EQ(db.table_count(), 1u);
    EXPECT_TRUE(db.find_table("users"));
    EXPECT_FALSE(db.find_table("missing"));

    EXPECT_TRUE(db.drop_table("users"));
    EXPECT_EQ(db.table_count(), 0u);
    EXPECT_FALSE(db.drop_table("users"));  // already gone
}

TEST(Database, RejectsDuplicateAndEmptyTableNames) {
    TempDir tmp;
    Database db{"id", "d", tmp.path()};

    EXPECT_TRUE(db.create_table("t"));
    EXPECT_FALSE(db.create_table("t"));  // duplicate
    EXPECT_FALSE(db.create_table(""));   // empty
    EXPECT_EQ(db.table_count(), 1u);
}

TEST(Database, CreateTableWritesFolderAndMeta) {
    TempDir tmp;
    Database db{"id", "d", tmp.path()};

    auto table = db.create_table("users");
    ASSERT_TRUE(table);
    EXPECT_TRUE(fs::is_directory(table->dir()));
    EXPECT_TRUE(fs::exists(table->dir() / "table.json"));
    // Folder name is the table's UUID id, not its human name.
    EXPECT_EQ(table->dir().filename().string(), table->id());
}

TEST(Database, DropTableRemovesFolder) {
    TempDir tmp;
    Database db{"id", "d", tmp.path()};
    auto table = db.create_table("t");
    const fs::path dir = table->dir();
    ASSERT_TRUE(fs::exists(dir));

    EXPECT_TRUE(db.drop_table("t"));
    EXPECT_FALSE(fs::exists(dir));
}

TEST(Database, LoadRebuildsTablesSortedByName) {
    TempDir tmp;
    {
        Database db{"id", "d", tmp.path()};
        db.create_table("zebra");
        db.create_table("alpha");
        db.create_table("mango");
    }
    Database reloaded{"id", "d", tmp.path()};
    reloaded.load();

    const std::vector<std::string> names = reloaded.table_names();
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "alpha");
    EXPECT_EQ(names[1], "mango");
    EXPECT_EQ(names[2], "zebra");
}
