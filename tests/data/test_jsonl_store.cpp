// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdint>
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

namespace {

// The chunk files' bytes, measured the slow way the counter replaces (#1195).
std::uint64_t chunk_file_bytes(const fs::path& dir) {
    std::uint64_t bytes = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".jsonl") bytes += entry.file_size();
    }
    return bytes;
}

}  // namespace

// #1195: stored_bytes() is a counter kept on every write, and equals the chunk
// files once pending writes are flushed: after adds, an update (counted before
// the flush writes it), a removal, a reload, and a legacy bare-row line that a
// rewrite puts in the current format. An id that JSON must escape is sized as
// it is written.
TEST(JsonlStore, StoredBytesFollowsEveryWriteWithoutReadingTheFiles) {
    TempDir tmp;
    {
        JsonlStore store{tmp.path()};
        ASSERT_TRUE(store.stored_bytes().has_value());
        EXPECT_EQ(*store.stored_bytes(), 0u);

        store.add("id-a", json{{"a", 1}});
        store.add("id-\"b\"", json{{"b", "two"}, {"nested", {1, 2, 3}}});
        store.add("id-c", json{{"c", nullptr}});
        EXPECT_EQ(*store.stored_bytes(), chunk_file_bytes(tmp.path()));

        store.update("id-\"b\"", json{{"b", "a much longer value than before"}});
        const std::uint64_t counted = *store.stored_bytes();
        EXPECT_NE(counted, chunk_file_bytes(tmp.path())) << "the write is deferred";
        store.flush();
        EXPECT_EQ(counted, chunk_file_bytes(tmp.path()));

        store.remove("id-a");
        EXPECT_EQ(*store.stored_bytes(), chunk_file_bytes(tmp.path()));
    }
    // A legacy bare row (no _zid wrapper), as an older node wrote it.
    {
        std::ofstream out(tmp.path() / "000000.jsonl", std::ios::app);
        out << json{{"legacy", true}}.dump() << '\n';
    }
    JsonlStore reloaded{tmp.path()};
    ASSERT_EQ(reloaded.size(), 3u);
    EXPECT_EQ(*reloaded.stored_bytes(), chunk_file_bytes(tmp.path()));
    // A rewrite of the chunk puts the legacy row in the current format.
    reloaded.update(reloaded.id_at(0), json{{"c", 3}});
    reloaded.flush();
    EXPECT_EQ(*reloaded.stored_bytes(), chunk_file_bytes(tmp.path()));
}
