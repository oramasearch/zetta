#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/storage/jsonl_store.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using zetta::storage::JsonlStore;
using zetta::test::TempDir;

TEST(JsonlStore, AddSizeAndRead) {
    TempDir tmp;
    JsonlStore store{tmp.path()};
    EXPECT_EQ(store.size(), 0u);
    EXPECT_STREQ(store.name(), "jsonl");

    store.add("id-a", json{{"a", 1}});
    store.add("id-b", json{{"b", 2}});
    ASSERT_EQ(store.size(), 2u);
    EXPECT_EQ(store.at(0)->at("a").get<int>(), 1);
    EXPECT_EQ(store.at(1)->at("b").get<int>(), 2);
    // Rows carry their id (the CRDT key) for idempotent reconciliation.
    EXPECT_EQ(store.id_at(0), "id-a");
    EXPECT_TRUE(store.contains("id-b"));
    EXPECT_FALSE(store.contains("id-missing"));
}

TEST(JsonlStore, PersistsAndReloads) {
    TempDir tmp;
    {
        JsonlStore store{tmp.path()};
        store.add("rx", json{{"x", "hello"}});
        store.add("ry", json{{"y", 42}});
    }
    JsonlStore reloaded{tmp.path()};
    ASSERT_EQ(reloaded.size(), 2u);
    EXPECT_EQ(reloaded.at(0)->at("x").get<std::string>(), "hello");
    EXPECT_EQ(reloaded.at(1)->at("y").get<int>(), 42);
    EXPECT_TRUE(reloaded.contains("rx"));  // ids survive a reload
    EXPECT_EQ(reloaded.id_at(1), "ry");
}

TEST(JsonlStore, ChunksEveryTenThousandRecords) {
    TempDir tmp;
    JsonlStore store{tmp.path()};
    for (int i = 0; i < 10001; ++i) store.add(std::to_string(i), json{{"i", i}});

    EXPECT_EQ(store.size(), 10001u);
    EXPECT_TRUE(fs::exists(tmp.path() / "000000.jsonl"));
    EXPECT_TRUE(fs::exists(tmp.path() / "000001.jsonl"));
    EXPECT_FALSE(fs::exists(tmp.path() / "000002.jsonl"));

    // The record straddling the chunk boundary survives a reload.
    JsonlStore reloaded{tmp.path()};
    ASSERT_EQ(reloaded.size(), 10001u);
    EXPECT_EQ(reloaded.at(9999)->at("i").get<int>(), 9999);
    EXPECT_EQ(reloaded.at(10000)->at("i").get<int>(), 10000);
}

TEST(JsonlStore, SkipsCorruptLinesOnLoad) {
    TempDir tmp;
    {
        JsonlStore store{tmp.path()};
        store.add("ok", json{{"ok", 1}});
    }
    {
        std::ofstream out(tmp.path() / "000000.jsonl", std::ios::app);
        out << "this is not json\n";
    }
    JsonlStore reloaded{tmp.path()};
    EXPECT_EQ(reloaded.size(), 1u);  // garbage line dropped, valid record kept
}
