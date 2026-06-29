#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/database.hpp"
#include "zetta/replication_strategy.hpp"

namespace fs = std::filesystem;
using zetta::Database;
using zetta::ReplicationStrategy;
using zetta::ReplicationStrategyKind;
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

TEST(Database, TablesDefaultToReplicateToAll) {
    TempDir tmp;
    Database db{"id", "d", tmp.path()};
    auto table = db.create_table("t");
    ASSERT_TRUE(table);
    EXPECT_TRUE(table->replication_strategy().is_default());
    EXPECT_EQ(table->replication_strategy().kind(),
              ReplicationStrategyKind::ReplicateToAll);
    // The default is omitted from table.json (byte-identical to before).
    std::ifstream in(table->dir() / "table.json");
    const auto meta = nlohmann::json::parse(in);
    EXPECT_FALSE(meta.contains("replication"));
}

TEST(Database, CreateTableWithExplicitStrategyPersistsAndReloads) {
    TempDir tmp;
    {
        Database db{"id", "d", tmp.path()};
        auto table = db.create_table(
            "t", ReplicationStrategy::partitioned_with_replicas(2));
        ASSERT_TRUE(table);
        EXPECT_EQ(table->replication_strategy(),
                  ReplicationStrategy::partitioned_with_replicas(2));
        std::ifstream in(table->dir() / "table.json");
        const auto meta = nlohmann::json::parse(in);
        ASSERT_TRUE(meta.contains("replication"));
        EXPECT_EQ(meta["replication"]["kind"], "partitioned_with_replicas");
        EXPECT_EQ(meta["replication"]["replication_factor"], 2);
    }
    Database reloaded{"id", "d", tmp.path()};
    reloaded.load();
    auto table = reloaded.find_table("t");
    ASSERT_TRUE(table);
    EXPECT_EQ(table->replication_strategy(),
              ReplicationStrategy::partitioned_with_replicas(2));
}

TEST(Database, TablesInheritDatabaseStrategyUnlessOverridden) {
    TempDir tmp;
    // A partitioned database: a table created with no explicit strategy inherits.
    Database db{"id", "d", tmp.path(), nullptr,
                ReplicationStrategy::partitioned()};
    EXPECT_EQ(db.replication_strategy(), ReplicationStrategy::partitioned());

    auto inherited = db.create_table("inherited");
    ASSERT_TRUE(inherited);
    EXPECT_EQ(inherited->replication_strategy(),
              ReplicationStrategy::partitioned());

    // An explicit strategy overrides the inherited default.
    auto overridden = db.create_table(
        "overridden", ReplicationStrategy::replicate_to_all());
    ASSERT_TRUE(overridden);
    EXPECT_TRUE(overridden->replication_strategy().is_default());
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
