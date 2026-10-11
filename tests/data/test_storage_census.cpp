// The storage census (docs/data/storage-census.md): every file under a node's data directory is
// counted once, in the kind its top-level entry is, and each object bucket on its own.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "temp_dir.hpp"
#include "zetta/data/storage_census.hpp"

namespace {

void write(const std::filesystem::path& path, std::size_t bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
}

}  // namespace

TEST(StorageCensus, CountsEachFileOnceInItsKind) {
    zetta::test::TempDir dir;
    const auto& root = dir.path();
    write(root / "db-1" / "database.json", 10);
    write(root / "db-1" / "orders" / "chunk-0.jsonl", 1000);
    write(root / "db-1" / "large_objects" / "blob-1", 300);
    write(root / "part-1" / "partition.json", 5);
    write(root / "part-1" / "rows.jsonl", 95);
    write(root / "model-1" / "model.json", 20);
    write(root / "model-1" / "weights.bin", 4000);
    write(root / "checkpoints" / "model-1" / "v1.bin", 500);
    write(root / "index-1" / "index.json", 7);
    write(root / "index-1" / "segment-0", 693);
    write(root / "run-1" / "run.json", 50);
    write(root / "study-1" / "study.json", 25);
    write(root / "experiments" / "e-1" / "trial.json", 25);
    write(root / "objects" / "lake_store" / "log-0", 800);
    write(root / "objects" / "photos" / "log-0", 90);
    write(root / "pubsub" / "topics" / "orders" / "segment-0", 60);
    write(root / "records" / "log-0", 400);
    write(root / "project-1" / "project.json", 30);
    write(root / "audit" / "trail-0", 11);
    write(root / "node_id", 36);
    write(root / "empty-dir" / "notes.txt", 3);

    const zetta::data::StorageCensus census = zetta::data::measure_storage(root);
    EXPECT_EQ(census.kinds.at("datasets"), 10 + 1000 + 5 + 95);
    EXPECT_EQ(census.kinds.at("files"), 300);
    EXPECT_EQ(census.kinds.at("models"), 20 + 4000 + 500);
    EXPECT_EQ(census.kinds.at("indexes"), 700);
    EXPECT_EQ(census.kinds.at("runs"), 100);
    EXPECT_EQ(census.kinds.at("buckets"), 890);
    EXPECT_EQ(census.kinds.at("streams"), 60);
    EXPECT_EQ(census.kinds.at("records"), 400);
    EXPECT_EQ(census.kinds.at("catalogue"), 30);
    EXPECT_EQ(census.kinds.at("system"), 11 + 36 + 3);
    EXPECT_EQ(census.buckets.at("lake_store"), 800);
    EXPECT_EQ(census.buckets.at("photos"), 90);
    std::uint64_t sum = 0;
    for (const auto& [kind, bytes] : census.kinds) sum += bytes;
    EXPECT_EQ(census.total_bytes, sum) << "each file once";
    EXPECT_EQ(census.kinds.size(), zetta::data::storage_kinds().size());
}

TEST(StorageCensus, AnEmptyOrMissingDirectoryHoldsNothing) {
    zetta::test::TempDir dir;
    const auto empty = zetta::data::measure_storage(dir.path());
    EXPECT_EQ(empty.total_bytes, 0U);
    EXPECT_EQ(empty.kinds.at("models"), 0U);
    const auto missing = zetta::data::measure_storage(dir.path() / "not-there");
    EXPECT_EQ(missing.total_bytes, 0U);
    EXPECT_EQ(missing.kinds.size(), zetta::data::storage_kinds().size());
}
