// Who may read the system.* views (#766, docs/governance/system-catalog-access.md).
//
// Before #766 every system table was answered before any identity check, so a
// caller with nothing but mcp:call read every user, role, policy (with the
// literal values in its conditions), token, the whole audit trail and every
// dataset name. Each table now has a reader class (SystemTableAccess in
// src/core/query/executor.cpp): public metadata, the rows of a read tool (whoever
// may call the tool), the datasets the caller may read, the caller's own audit
// rows, or admin only. The trusted local session and an admin read every table
// whole.
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"

namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::Resource;
using zetta::query::QueryService;
using zetta::test::TempDir;

constexpr const char* kOwnerSecret = "system-catalog-owner-secret";

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    cfg.preflight = false;
    return cfg;
}

bool refused(const QueryService::Outcome& result) {
    return !result.ok() && result.error.find("permission denied") != std::string::npos;
}

// A node with its owner seeded (an admin), the built-in roles, a policy whose
// condition carries a literal, and two partitioned datasets: app.open and
// app.hidden.
struct SystemCatalogAccess : ::testing::Test {
    TempDir dir;
    Node node{config_for(dir)};
    QueryService local{node};
    std::string owner;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        const auto boot = node.bootstrap_owner(kOwnerSecret);
        ASSERT_TRUE(boot.performed) << boot.reason;
        owner = boot.user_id;
        ASSERT_FALSE(owner.empty());
        node.ensure_builtin_roles();
        for (const char* table : {"open", "hidden"}) {
            zetta::data::AddDatasetOptions options;
            options.database = "app";
            options.table_name = table;
            std::vector<nlohmann::json> rows = {{{"id", 1}, {"region", "eu"}},
                                                {{"id", 2}, {"region", "us"}}};
            const auto added = zetta::data::add_dataset_rows(node, rows, options);
            ASSERT_TRUE(added.ok()) << added.error;
            ASSERT_FALSE(added.partitions.empty()) << table;
        }
        const auto policy = local.run(
            "CREATE POLICY eu_only ON app.open FOR SELECT "
            "USING (region = 'restricted-eu');",
            "");
        ASSERT_TRUE(policy.ok()) << policy.error;
    }
    void TearDown() override { node.stop(); }

    std::string user_with_roles(const std::string& name,
                                const std::vector<std::string>& roles) {
        auto user = node.create_user(name);
        EXPECT_TRUE(user) << name;
        if (!user) return {};
        for (const std::string& role_name : roles) {
            auto role = node.find_role(role_name);
            EXPECT_TRUE(role) << role_name;
            if (role) node.assign_role(user->id(), role->id());
        }
        return user->id();
    }
    // A user whose only rights are sql:read on app.open and mcp:call on `tools`.
    std::string analyst(const std::vector<std::string>& tools = {"sql"}) {
        node.create_role("analyst");
        EXPECT_TRUE(node.grant_scoped_permissions("analyst", {"sql:read"},
                                                  Resource::table("app", "open")));
        for (const std::string& tool : tools) {
            EXPECT_TRUE(node.grant_scoped_permissions("analyst", {"mcp:call"},
                                                      Resource::tool(tool)));
        }
        return user_with_roles("ana", {"analyst"});
    }
    QueryService as(const std::string& user_id) {
        return QueryService{node, nullptr, user_id, /*acting_is_user=*/true};
    }
    std::set<std::string> column(const QueryService::Outcome& result,
                                 const std::string& name) {
        std::set<std::string> values;
        for (const auto& row : result.rows) {
            values.insert(row.value(name, std::string()));
        }
        return values;
    }
};

// The governance registry: a reader (flat sql:read + mcp:call) is refused every
// table of it, the admin and the local session read them all.
TEST_F(SystemCatalogAccess, AReaderCannotReadTheGovernanceRegistry) {
    const std::string reader = user_with_roles("rita", {"reader"});
    for (const char* view :
         {"system.policies", "system.users", "system.roles", "system.user_roles",
          "system.tokens", "system.keys", "system.principals"}) {
        const std::string sql = std::string("SELECT * FROM ") + view + ";";
        const auto as_reader = as(reader).run(sql, "");
        EXPECT_TRUE(refused(as_reader)) << view << ": " << as_reader.error;
        EXPECT_NE(as_reader.error.find("cluster:admin"), std::string::npos) << view;
        EXPECT_TRUE(as_reader.rows.empty()) << view;

        const auto as_owner = as(owner).run(sql, "");
        EXPECT_TRUE(as_owner.ok()) << view << ": " << as_owner.error;
        const auto as_local = local.run(sql, "");
        EXPECT_TRUE(as_local.ok()) << view << ": " << as_local.error;
    }
    // The policy text, with its literal, reaches the admin only.
    const auto policies = as(owner).run("SELECT * FROM system.policies;", "");
    ASSERT_TRUE(policies.ok()) << policies.error;
    EXPECT_NE(nlohmann::json(policies.rows).dump().find("restricted-eu"),
              std::string::npos);
}

// The node's internals are admin-only too. A WHERE or an aggregate does not get
// round the refusal.
TEST_F(SystemCatalogAccess, TheNodesInternalsAreAdminOnly) {
    const std::string reader = user_with_roles("rita", {"reader"});
    for (const char* sql :
         {"SELECT * FROM system.usage;", "SELECT * FROM system.spans;",
          "SELECT * FROM system.memory;", "SELECT * FROM system.large_objects;",
          "SELECT count(*) FROM system.users;",
          "SELECT * FROM system.users WHERE name = 'rita';",
          "SELECT * FROM system.zetta_principals;"}) {
        EXPECT_TRUE(refused(as(reader).run(sql, ""))) << sql;
        EXPECT_TRUE(as(owner).run(sql, "").ok()) << sql;
    }
}

// Public metadata holds no workspace data: anyone who can run SQL reads it.
TEST_F(SystemCatalogAccess, PublicMetadataIsReadableByAnyone) {
    const std::string ana = analyst();
    for (const char* sql : {"SELECT * FROM system.tables;",
                            "SELECT * FROM system.test_cases;",
                            "SELECT * FROM system.cloud_providers;"}) {
        const auto result = as(ana).run(sql, "");
        EXPECT_TRUE(result.ok()) << sql << ": " << result.error;
        EXPECT_FALSE(result.rows.empty()) << sql;
    }
    // An unknown system table is still the catalog error, not a refusal.
    const auto unknown = as(ana).run("SELECT * FROM system.nope;", "");
    EXPECT_FALSE(unknown.ok());
    EXPECT_NE(unknown.error.find("no such system table"), std::string::npos);
}

// system.datasets and system.dataset_partitions list only what the caller may
// read: the analyst sees app.open and its partitions, never app.hidden.
TEST_F(SystemCatalogAccess, DatasetsAndPartitionsAreNarrowedToWhatTheCallerMayRead) {
    const std::string ana = analyst();
    const auto datasets = as(ana).run("SELECT * FROM system.datasets;", "");
    ASSERT_TRUE(datasets.ok()) << datasets.error;
    EXPECT_EQ(column(datasets, "dataset"), std::set<std::string>{"open"});
    EXPECT_EQ(nlohmann::json(datasets.rows).dump().find("hidden"), std::string::npos);

    const auto all = as(owner).run("SELECT * FROM system.datasets;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    EXPECT_EQ(column(all, "dataset"), (std::set<std::string>{"hidden", "open"}));

    std::string open_id;
    std::string hidden_id;
    for (const auto& row : all.rows) {
        (row.at("dataset") == "open" ? open_id : hidden_id) =
            row.value("table_id", std::string());
    }
    const auto partitions =
        as(ana).run("SELECT * FROM system.dataset_partitions;", "");
    ASSERT_TRUE(partitions.ok()) << partitions.error;
    ASSERT_FALSE(partitions.rows.empty());
    EXPECT_EQ(column(partitions, "table"), std::set<std::string>{open_id});

    const auto every_partition =
        as(owner).run("SELECT * FROM system.dataset_partitions;", "");
    ASSERT_TRUE(every_partition.ok()) << every_partition.error;
    EXPECT_EQ(column(every_partition, "table"),
              (std::set<std::string>{open_id, hidden_id}));

    // A count is narrowed the same way.
    const auto counted = as(ana).run("SELECT count(*) AS n FROM system.datasets;", "");
    ASSERT_TRUE(counted.ok()) << counted.error;
    ASSERT_EQ(counted.rows.size(), 1u);
    EXPECT_EQ(counted.rows[0].at("n"), 1);
}

// #847 step 6: two people asking the trail the same question each get their own
// rows; the admin gets both. The same holds for system.activity.
TEST_F(SystemCatalogAccess, EachPersonReadsOnlyTheirOwnAuditRows) {
    const std::string ada = user_with_roles("ada", {"reader"});
    const std::string bo = user_with_roles("bo", {"reader"});
    for (const auto& [actor, target] :
         {std::pair{ada, std::string("sql:table:app.open")},
          std::pair{bo, std::string("sql:table:app.hidden")}}) {
        zetta::governance::AuditEventValue event;
        event.action = zetta::governance::AuditAction::DataAccess;
        event.actor = actor;
        event.target = target;
        event.allowed = true;
        event.detail = "read";
        node.record_audit_event(event);
    }
    for (const char* view : {"system.audit", "system.activity"}) {
        const std::string sql = std::string("SELECT * FROM ") + view + ";";
        const auto as_ada = as(ada).run(sql, "");
        ASSERT_TRUE(as_ada.ok()) << view << ": " << as_ada.error;
        EXPECT_EQ(column(as_ada, "actor"), std::set<std::string>{ada}) << view;
        EXPECT_EQ(column(as_ada, "target"),
                  std::set<std::string>{"sql:table:app.open"})
            << view;

        const auto as_bo = as(bo).run(sql, "");
        ASSERT_TRUE(as_bo.ok()) << view << ": " << as_bo.error;
        EXPECT_EQ(column(as_bo, "actor"), std::set<std::string>{bo}) << view;

        const auto as_admin = as(owner).run(sql, "");
        ASSERT_TRUE(as_admin.ok()) << view << ": " << as_admin.error;
        const std::set<std::string> actors = column(as_admin, "actor");
        EXPECT_TRUE(actors.contains(ada) && actors.contains(bo)) << view;
    }
}

// A table that mirrors a capability follows that capability's scope: a user
// granted only the sql tool cannot read system.models, one also granted
// list_models can, and system.connections and system.deployments also need
// list_connections' and deploy's own orchestration rights, which the built-in
// reader does not hold.
TEST_F(SystemCatalogAccess, AToolTableFollowsTheToolsOwnScope) {
    const std::string ana = analyst({"sql"});
    const auto models = as(ana).run("SELECT * FROM system.models;", "");
    EXPECT_TRUE(refused(models)) << models.error;
    EXPECT_NE(models.error.find("list_models"), std::string::npos) << models.error;
    EXPECT_TRUE(refused(as(ana).run("SELECT * FROM system.runs;", "")));

    ASSERT_TRUE(node.grant_scoped_permissions("analyst", {"mcp:call"},
                                              Resource::tool("list_models")));
    EXPECT_TRUE(as(ana).run("SELECT * FROM system.models;", "").ok());
    EXPECT_TRUE(refused(as(ana).run("SELECT * FROM system.runs;", "")));

    const std::string reader = user_with_roles("rita", {"reader"});
    for (const char* sql : {"SELECT * FROM system.models;", "SELECT * FROM system.runs;",
                            "SELECT * FROM system.jobs;", "SELECT * FROM system.indexes;",
                            "SELECT * FROM system.ledger;",
                            "SELECT * FROM system.consent_requests;"}) {
        const auto result = as(reader).run(sql, "");
        EXPECT_TRUE(result.ok()) << sql << ": " << result.error;
    }
    EXPECT_TRUE(refused(as(reader).run("SELECT * FROM system.connections;", "")));
    EXPECT_TRUE(refused(as(reader).run("SELECT * FROM system.pubsub_topics;", "")));
    // system.deployments follows deploy, which writes it: orchestration:deploy.
    EXPECT_TRUE(refused(as(reader).run("SELECT * FROM system.deployments;", "")));
    EXPECT_TRUE(as(owner).run("SELECT * FROM system.deployments;", "").ok());
    EXPECT_TRUE(as(owner).run("SELECT * FROM system.connections;", "").ok());
}

// A user that is disabled, or unknown, reads nothing but public metadata.
TEST_F(SystemCatalogAccess, AnUnknownUserReadsOnlyPublicMetadata) {
    QueryService ghost{node, nullptr, "usr:does-not-exist", /*acting_is_user=*/true};
    EXPECT_TRUE(ghost.run("SELECT * FROM system.tables;", "").ok());
    EXPECT_TRUE(refused(ghost.run("SELECT * FROM system.models;", "")));
    EXPECT_TRUE(refused(ghost.run("SELECT * FROM system.users;", "")));
    EXPECT_TRUE(refused(ghost.run("SELECT * FROM system.consent_requests;", "")));
    const auto datasets = ghost.run("SELECT * FROM system.datasets;", "");
    ASSERT_TRUE(datasets.ok()) << datasets.error;
    EXPECT_TRUE(datasets.rows.empty());
    const auto audit = ghost.run("SELECT * FROM system.audit;", "");
    ASSERT_TRUE(audit.ok()) << audit.error;
    EXPECT_TRUE(audit.rows.empty());
}

}  // namespace
