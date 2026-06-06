#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/database.hpp"
#include "zetta/node.hpp"
#include "zetta/table.hpp"

using nlohmann::json;
using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

}  // namespace

TEST(Node, CreateFindDropRenameDatabase) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    ASSERT_TRUE(node.create_database("app"));
    EXPECT_FALSE(node.create_database("app"));  // duplicate
    EXPECT_FALSE(node.create_database(""));      // empty
    EXPECT_EQ(node.database_count(), 1u);
    EXPECT_TRUE(node.find_database("app"));

    EXPECT_TRUE(node.rename_database("app", "prod"));
    EXPECT_TRUE(node.find_database("prod"));
    EXPECT_FALSE(node.find_database("app"));
    EXPECT_FALSE(node.rename_database("nope", "x"));  // missing source

    EXPECT_TRUE(node.drop_database("prod"));
    EXPECT_EQ(node.database_count(), 0u);
    EXPECT_FALSE(node.drop_database("prod"));  // already gone

    node.stop();
}

TEST(Node, RenameRejectsNameClash) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    node.create_database("a");
    node.create_database("b");

    EXPECT_FALSE(node.rename_database("a", "b"));  // target name taken
    EXPECT_TRUE(node.find_database("a"));
    EXPECT_TRUE(node.find_database("b"));

    node.stop();
}

TEST(Node, PersistsAndReloadsAcrossInstances) {
    TempDir tmp;
    {
        Node a{config_for(tmp)};
        a.start();
        auto db = a.create_database("shop");
        auto items = db->create_table("items");
        items->add_row(json{{"sku", "A1"}});
        items->add_row(json{{"sku", "B2"}});
        a.stop();
    }

    Node b{config_for(tmp)};
    b.start();
    EXPECT_EQ(b.database_count(), 1u);
    auto db = b.find_database("shop");
    ASSERT_TRUE(db);
    auto items = db->find_table("items");
    ASSERT_TRUE(items);
    EXPECT_EQ(items->row_count(), 2u);
    EXPECT_EQ(items->store().at(0)["sku"].get<std::string>(), "A1");
    b.stop();
}

TEST(Node, RenamePreservesTableData) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto db = node.create_database("app");
    db->create_table("t")->add_row(json{{"n", 1}});

    ASSERT_TRUE(node.rename_database("app", "prod"));
    auto prod = node.find_database("prod");
    ASSERT_TRUE(prod);
    auto table = prod->find_table("t");
    ASSERT_TRUE(table);
    EXPECT_EQ(table->row_count(), 1u);

    node.stop();
}
