// zetta#1392 (LSR-072): virtual tables never read around table grants, row-level
// security or the connection-store refusal.
//
// A virtual table projects raw replicated values from a CRDT key family. Before
// this fix, flat sql:ddl (the built-in writer role) could declare one over the
// `row:` family and read every row of every table, columns and policies
// notwithstanding, and any principal with the SQL tool could read an existing
// definition. Now:
//   - creating one needs cluster:admin as well as flat sql:ddl, and reading one
//     needs cluster:admin (the trusted local session too), like system.nodes;
//   - no one, admin included, declares or reads one over a reserved family
//     (table rows and structure, large objects, identities, credentials,
//     governance, scoped record/object/metric families, the definitions).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/virtual_table.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig config;
    config.data_dir = tmp.path().string();
    config.preflight = false;
    return config;
}

struct VirtualTableAccess : ::testing::Test {
    TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    Node node{config_for(dir), &replication};
    QueryService local{node};  // the trusted local session builds the fixture

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        replication.set("thing:t1", nlohmann::json{{"label", "one"}});
        replication.set("thing:t2", nlohmann::json{{"label", "two"}});
        for (const char* sql :
             {"CREATE DATABASE app;", "CREATE TABLE app.docs (id int);",
              "INSERT INTO app.docs (id, owner, secret) VALUES (1, 'ada', 'the-secret');"}) {
            const auto result = local.run(sql, "");
            ASSERT_TRUE(result.ok()) << result.error << "  (" << sql << ")";
        }
    }
    void TearDown() override { node.stop(); }

    std::string user_with_role(const std::string& name, const std::string& role_name) {
        auto user = node.create_user(name);
        auto role = node.find_role(role_name);
        EXPECT_TRUE(user && role) << role_name;
        if (user && role) node.assign_role(user->id(), role->id());
        return user ? user->id() : std::string{};
    }

    static std::string create_over(const std::string& name, const std::string& prefix) {
        return "CREATE VIRTUAL TABLE " + name + " OVER CRDT '" + prefix +
               "' (id TEXT DURABLE KEY, data TEXT DURABLE, label TEXT DURABLE);";
    }
};

// The exploit from the review: the writer role declares a virtual table over the
// row family and reads every table's rows through it.
TEST_F(VirtualTableAccess, TheWriterRoleCannotDeclareAVirtualTable) {
    QueryService as_writer{node, nullptr, user_with_role("wes", "writer"),
                           /*acting_is_user=*/true};
    const auto created = as_writer.run(create_over("leak", "row:"), "");
    EXPECT_FALSE(created.ok());
    EXPECT_NE(created.error.find("permission denied"), std::string::npos) << created.error;
    EXPECT_FALSE(node.find_virtual_table("leak"));

    // Not over a harmless family either: sql:ddl alone does not make one.
    EXPECT_FALSE(as_writer.run(create_over("mine", "thing:"), "").ok());
    EXPECT_FALSE(node.find_virtual_table("mine"));
}

TEST_F(VirtualTableAccess, NoOneDeclaresOneOverAReservedFamily) {
    QueryService as_admin{node, nullptr, user_with_role("ann", "admin"),
                          /*acting_is_user=*/true};
    for (const std::string prefix :
         {"row:", "row:abc", "r", "ro", "tbl:", "lo:", "token:", "user", "role:", "key:",
          "principal:", "policy:", "audit:", "vtable:", "record_collection:"}) {
        SCOPED_TRACE(prefix);
        for (QueryService* session : {&local, &as_admin}) {
            const auto created = session->run(create_over("reserved", prefix), "");
            EXPECT_FALSE(created.ok());
            EXPECT_NE(created.error.find("reserved key family"), std::string::npos)
                << created.error;
        }
    }
    EXPECT_FALSE(node.find_virtual_table("reserved"));

    // A family of its own is fine for an admin.
    const auto created = as_admin.run(create_over("things", "thing:"), "");
    ASSERT_TRUE(created.ok()) << created.error;
    const auto rows = as_admin.run("SELECT * FROM things;", "");
    ASSERT_TRUE(rows.ok()) << rows.error;
    EXPECT_EQ(rows.rows.size(), 2u);
}

TEST_F(VirtualTableAccess, OnlyAnAdminReadsAVirtualTable) {
    ASSERT_TRUE(local.run(create_over("things", "thing:"), "").ok());

    QueryService as_reader{node, nullptr, user_with_role("rita", "reader"),
                           /*acting_is_user=*/true};
    const auto refused = as_reader.run("SELECT * FROM things;", "");
    EXPECT_FALSE(refused.ok());
    EXPECT_NE(refused.error.find("admin only"), std::string::npos) << refused.error;
    EXPECT_TRUE(refused.rows.empty());

    QueryService as_writer{node, nullptr, user_with_role("wes", "writer"), true};
    EXPECT_FALSE(as_writer.run("SELECT label FROM things WHERE id = 't1';", "").ok());

    QueryService as_admin{node, nullptr, user_with_role("ann", "admin"), true};
    EXPECT_TRUE(as_admin.run("SELECT * FROM things;", "").ok());
    EXPECT_TRUE(local.run("SELECT * FROM things;", "").ok());
}

// A definition over a reserved family that already exists (declared before this
// check, or replicated from an older node) is never read, by anyone.
TEST_F(VirtualTableAccess, AnExistingDefinitionOverAReservedFamilyIsNeverRead) {
    zetta::data::VirtualTable leak;
    leak.name = "old_leak";
    leak.crdt_prefix = "row:";
    leak.columns = {{.name = "id", .type = "TEXT",
                     .tier = zetta::data::VirtualColumnTier::Durable, .is_key = true},
                    {.name = "data", .type = "TEXT",
                     .tier = zetta::data::VirtualColumnTier::Durable}};
    ASSERT_TRUE(node.create_virtual_table(leak));
    ASSERT_TRUE(node.find_virtual_table("old_leak"));

    QueryService as_admin{node, nullptr, user_with_role("ann", "admin"), true};
    for (QueryService* session : {&local, &as_admin}) {
        const auto read = session->run("SELECT * FROM old_leak;", "");
        EXPECT_FALSE(read.ok());
        EXPECT_NE(read.error.find("reserved key family"), std::string::npos) << read.error;
        EXPECT_TRUE(read.rows.empty());
        EXPECT_EQ(read.error.find("the-secret"), std::string::npos);
    }
}

}  // namespace
