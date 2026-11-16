// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "temp_dir.hpp"
#include "zetta/data/large_object.hpp"

namespace fs = std::filesystem;
using zetta::data::LargeObjectStore;
using zetta::test::TempDir;

namespace {

fs::path write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

}  // namespace

TEST(LargeObjectStore, ImportReadAndList) {
    TempDir tmp;
    const fs::path src = write_file(tmp.path() / "hello.txt", "hello world");

    LargeObjectStore store{tmp.path() / "lo"};
    const auto lo = store.import_file(src);
    ASSERT_TRUE(lo.has_value());
    EXPECT_EQ(lo->name, "hello.txt");
    EXPECT_EQ(lo->size, 11u);
    EXPECT_EQ(store.count(), 1u);

    const auto bytes = store.read(lo->id);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(*bytes, "hello world");

    const auto all = store.list();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].id, lo->id);
}

TEST(LargeObjectStore, StoresArbitraryBinaryContent) {
    TempDir tmp;
    // Length-counted pieces so hex escapes don't greedily eat following letters.
    const std::string binary =
        std::string("\x00\x01\x02\xFF\xFE", 5) + "binary" +
        std::string("\x00", 1) + "here";
    const fs::path src = write_file(tmp.path() / "blob.bin", binary);

    LargeObjectStore store{tmp.path() / "lo"};
    const auto lo = store.import_file(src);
    ASSERT_TRUE(lo.has_value());
    EXPECT_EQ(lo->size, binary.size());
    const auto bytes = store.read(lo->id);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(*bytes, binary);  // round-trips with embedded NULs
}

TEST(LargeObjectStore, ExportWritesBytesBack) {
    TempDir tmp;
    const fs::path src = write_file(tmp.path() / "doc.txt", "exported");
    LargeObjectStore store{tmp.path() / "lo"};
    const auto lo = store.import_file(src);
    ASSERT_TRUE(lo.has_value());

    const fs::path dst = tmp.path() / "out.txt";
    ASSERT_TRUE(store.export_file(lo->id, dst));
    std::ifstream in(dst, std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    EXPECT_EQ(got, "exported");
}

TEST(LargeObjectStore, UnlinkRemovesObject) {
    TempDir tmp;
    const fs::path src = write_file(tmp.path() / "x", "data");
    LargeObjectStore store{tmp.path() / "lo"};
    const auto lo = store.import_file(src);
    ASSERT_TRUE(lo.has_value());

    EXPECT_TRUE(store.unlink(lo->id));
    EXPECT_EQ(store.count(), 0u);
    EXPECT_FALSE(store.read(lo->id).has_value());
    EXPECT_FALSE(store.unlink(lo->id));  // already gone
}

TEST(LargeObjectStore, PersistsAndReloads) {
    TempDir tmp;
    const fs::path lo_dir = tmp.path() / "lo";
    std::string id;
    {
        LargeObjectStore store{lo_dir};
        const auto lo = store.import_file(write_file(tmp.path() / "a.txt", "abc"));
        ASSERT_TRUE(lo.has_value());
        id = lo->id;
    }
    LargeObjectStore reloaded{lo_dir};
    EXPECT_EQ(reloaded.count(), 1u);
    const auto bytes = reloaded.read(id);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(*bytes, "abc");
}

TEST(LargeObjectStore, MissingIdsAreHandled) {
    TempDir tmp;
    LargeObjectStore store{tmp.path() / "lo"};
    EXPECT_FALSE(store.read("nope").has_value());
    EXPECT_FALSE(store.export_file("nope", tmp.path() / "x"));
    EXPECT_FALSE(store.import_file(tmp.path() / "does-not-exist").has_value());
}
