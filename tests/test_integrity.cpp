#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/table.hpp"
#include "zetta/util/uuid.hpp"

using zetta::IntegrityReport;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ReplicationManager;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

std::filesystem::path write_file(const TempDir& dir, const std::string& name,
                                 const std::string& content) {
    const std::filesystem::path path = dir.path() / name;
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path;
}

}  // namespace

TEST(Integrity, CleanDataIsReportedOk) {
    TempDir tmp, src;
    Node node{config_for(tmp)};
    node.start();
    auto db = node.create_database("shop");
    auto items = db->create_table("items");
    items->add_row(nlohmann::json{{"sku", "A1"}});
    items->add_row(nlohmann::json{{"sku", "B2"}});
    db->large_objects().import_file(write_file(src, "f.bin", "hello world"));

    const IntegrityReport report = node.check_integrity();
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.databases, 1u);
    EXPECT_EQ(report.tables, 1u);
    EXPECT_EQ(report.rows, 2u);
    EXPECT_EQ(report.large_objects, 1u);
    EXPECT_EQ(report.large_object_bodies_present, 1u);
    EXPECT_EQ(report.corrupt_large_objects, 0u);
    node.stop();
}

TEST(Integrity, DetectsCorruptLargeObjectBody) {
    TempDir tmp, src;
    std::string db_id, lo_id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto db = node.create_database("media");
        db_id = db->id();
        auto lo = db->large_objects().import_file(
            write_file(src, "photo.bin", "the original bytes"));
        ASSERT_TRUE(lo.has_value());
        lo_id = lo->id;
        node.stop();
    }
    // Corrupt the body on disk (simulate bit-rot), keeping meta.json intact.
    const std::filesystem::path blob =
        tmp.path() / db_id / "large_objects" / lo_id / "blob";
    {
        std::ofstream out(blob, std::ios::binary | std::ios::trunc);
        out << "TAMPERED";
    }

    Node reloaded{config_for(tmp)};
    reloaded.start();  // preflight runs here
    const IntegrityReport report = reloaded.integrity();
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.corrupt_large_objects, 1u);
    reloaded.stop();
}

TEST(Integrity, DetectsDanglingReferences) {
    TempDir tmp;
    const std::string node_id = zetta::new_uuid();
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    // A row whose table doesn't exist, and a table whose database doesn't exist.
    replication.set("row:" + zetta::new_uuid(),
                    nlohmann::json{{"tbl", "ghost-table"}, {"data", nlohmann::json::object()}});
    replication.set("tbl:" + zetta::new_uuid(),
                    nlohmann::json{{"db", "ghost-db"}, {"name", "orphan"}});

    const IntegrityReport report = node.check_integrity();
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.dangling_rows, 1u);
    EXPECT_EQ(report.dangling_tables, 1u);
    node.stop();
}
