// SPDX-License-Identifier: MIT

// Row-level security (FEAT-9 / #748, epic #743 item 5) — Postgres-style RLS: the
// system.policies entity (create/find/drop + JSON boundary + replication), the
// admin/local/legacy bypass decision, and the executor's predicate rewrite:
//   - a SELECT USING (owner = current_user()) limits a non-admin to its own rows;
//   - a write violating WITH CHECK is rejected;
//   - admin sees every row (bypass);
//   - a role-scoped policy applies only to that role (deny-by-default otherwise);
//   - a 'role' = ANY(current_roles()) predicate wires the role set in;
//   - policies replicate to a peer through the CRDT.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/policy.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::PolicyValue;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    cfg.preflight = false;
    return cfg;
}

// The structured form of `owner = current_user()` (the shape the executor stores).
nlohmann::json owner_is_current_user() {
    return nlohmann::json{{"lhs", {{"kind", "column"}, {"column", "owner"}}},
                          {"op", "eq"},
                          {"rhs", {{"kind", "current_user"}}}};
}

}  // namespace

// ===== system.policies value: the trust boundary ============================

TEST(RowSecurityPolicyJson, RoundTripsThroughTheBoundary) {
    PolicyValue value;
    value.name = "own_docs";
    value.database = "app";
    value.table = "docs";
    value.command = "select";
    value.using_predicate = owner_is_current_user();
    value.roles = {"analyst"};
    value.created = "2026-07-10T00:00:00.000Z";

    const nlohmann::json doc = zetta::governance::policy_value_to_json(value);
    const auto parsed = zetta::governance::policy_value_from_json(doc);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, value);

    // Malformed values are rejected, not silently accepted (a broken policy must
    // never quietly disable RLS).
    EXPECT_FALSE(zetta::governance::policy_value_from_json(nlohmann::json::array()));
    EXPECT_FALSE(zetta::governance::policy_value_from_json(
        nlohmann::json{{"name", "x"}}));  // missing database/table/command
    EXPECT_FALSE(zetta::governance::policy_value_from_json(
        nlohmann::json{{"name", "x"},
                       {"database", "a"},
                       {"table", "t"},
                       {"command", "bogus"}}));  // unknown command
    EXPECT_FALSE(zetta::governance::policy_value_from_json(
        nlohmann::json{{"name", "x"},
                       {"database", "a"},
                       {"table", "t"},
                       {"command", "select"},
                       {"using", 7}}));  // predicate must be an object
}

// ===== Node: create / find / drop + bypass decision =========================

struct RowSecurityNode : ::testing::Test {
    TempDir dir;
    Node node{config_for(dir)};

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
    }
    void TearDown() override { node.stop(); }

    std::string user_with_roles(const std::string& name,
                                const std::vector<std::string>& role_names) {
        auto user = node.create_user(name);
        EXPECT_TRUE(user);
        for (const std::string& role_name : role_names) {
            auto role = node.find_role(role_name);
            EXPECT_TRUE(role) << role_name;
            if (user && role) node.assign_role(user->id(), role->id());
        }
        return user ? user->id() : std::string{};
    }
};

TEST_F(RowSecurityNode, CreateFindDropPolicy) {
    PolicyValue value;
    value.name = "p1";
    value.database = "app";
    value.table = "docs";
    value.command = "all";
    value.using_predicate = owner_is_current_user();

    auto policy = node.create_policy(value);
    ASSERT_TRUE(policy);
    EXPECT_EQ(node.policy_count(), 1u);
    EXPECT_TRUE(node.find_policy("p1"));
    EXPECT_FALSE(policy->value().created.empty());  // stamped on create

    EXPECT_FALSE(node.create_policy(value));  // name clash
    PolicyValue nameless = value;
    nameless.name.clear();
    EXPECT_FALSE(node.create_policy(nameless));  // empty name

    EXPECT_TRUE(node.drop_policy("p1"));
    EXPECT_FALSE(node.find_policy("p1"));
    EXPECT_FALSE(node.drop_policy("p1"));  // already gone
}

TEST_F(RowSecurityNode, BypassDecisionMirrorsObjectScoping) {
    const std::string admin_id = user_with_roles("boss", {"admin"});
    const std::string writer_id = user_with_roles("wanda", {"writer"});

    EXPECT_TRUE(node.bypasses_row_security(admin_id, true));    // admin bypasses
    EXPECT_FALSE(node.bypasses_row_security(writer_id, true));  // a writer does NOT
    EXPECT_TRUE(node.bypasses_row_security("", false));         // trusted-local session
    // A legacy AccessKey bypasses only with cluster:admin of its own (#762).
    EXPECT_FALSE(node.bypasses_row_security("legacy-key", false));
    EXPECT_FALSE(node.bypasses_row_security("no-such-user", true));  // unknown user
}

// ===== end-to-end through the SQL executor ==================================

struct RowSecuritySql : ::testing::Test {
    TempDir dir;
    Node node{config_for(dir)};

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        // Seed as the trusted-local session (empty acting key -> bypass). Owner
        // values match the user NAMES that current_user() resolves to.
        QueryService local{node, nullptr, "", /*acting_is_user=*/false};
        ASSERT_TRUE(local.run("CREATE DATABASE app;", "").ok());
        ASSERT_TRUE(local.run("CREATE TABLE app.docs (id INT);", "").ok());
        ASSERT_TRUE(
            local.run("INSERT INTO app.docs (id, owner, title) "
                      "VALUES (1, 'ada', 'a1');", "").ok());
        ASSERT_TRUE(
            local.run("INSERT INTO app.docs (id, owner, title) "
                      "VALUES (2, 'bo', 'b1');", "").ok());
        ASSERT_TRUE(
            local.run("INSERT INTO app.docs (id, owner, title) "
                      "VALUES (3, 'ada', 'a2');", "").ok());
    }
    void TearDown() override { node.stop(); }

    std::string user_with_roles(const std::string& name,
                                const std::vector<std::string>& role_names) {
        auto user = node.create_user(name);
        EXPECT_TRUE(user);
        for (const std::string& role_name : role_names) {
            auto role = node.find_role(role_name);
            EXPECT_TRUE(role) << role_name;
            if (user && role) node.assign_role(user->id(), role->id());
        }
        return user ? user->id() : std::string{};
    }

    void create_policy(const std::string& sql) {
        QueryService admin{node, nullptr, "", false};  // local session is trusted
        auto r = admin.run(sql, "");
        ASSERT_TRUE(r.ok()) << r.error << "  (" << sql << ")";
    }
};

// USING (owner = current_user()) limits a non-admin's SELECT to its own rows.
TEST_F(RowSecuritySql, UsingLimitsNonAdminToOwnRows) {
    create_policy(
        "CREATE POLICY own_docs ON app.docs FOR SELECT "
        "USING (owner = current_user());");
    // A writer carries flat sql:read (passes the #747 column gate) but is NOT an
    // admin, so it is subject to RLS.
    const std::string ada = user_with_roles("ada", {"writer"});
    QueryService as_ada{node, nullptr, ada, /*acting_is_user=*/true};

    auto r = as_ada.run("SELECT * FROM app.docs;", "");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 2u);  // ada owns rows 1 and 3
    for (const auto& row : r.rows) EXPECT_EQ(row["owner"], "ada");
}

// admin sees every row regardless of policy (bypass).
TEST_F(RowSecuritySql, AdminBypassesRls) {
    create_policy(
        "CREATE POLICY own_docs ON app.docs FOR SELECT "
        "USING (owner = current_user());");
    const std::string boss = user_with_roles("boss", {"admin"});
    QueryService as_boss{node, nullptr, boss, true};

    auto r = as_boss.run("SELECT * FROM app.docs;", "");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.rows.size(), 3u);  // all rows
}

// A write violating WITH CHECK is rejected; a compliant one is accepted.
TEST_F(RowSecuritySql, WithCheckRejectsForeignWrite) {
    create_policy(
        "CREATE POLICY own_writes ON app.docs FOR INSERT "
        "WITH CHECK (owner = current_user());");
    const std::string ada = user_with_roles("ada", {"writer"});
    QueryService as_ada{node, nullptr, ada, true};

    EXPECT_TRUE(
        as_ada.run("INSERT INTO app.docs (id, owner) VALUES (10, 'ada');", "")
            .ok());
    auto denied =
        as_ada.run("INSERT INTO app.docs (id, owner) VALUES (11, 'bo');", "");
    EXPECT_FALSE(denied.ok());
    EXPECT_NE(denied.error.find("WITH CHECK"), std::string::npos) << denied.error;
}

// #765: a policy missing the predicate its command enforces fails closed. CREATE
// POLICY refuses one, so it enters the way a replicated record or an edited
// policy.json does: through policy_value_from_json. It is kept (so the table stays
// engaged) and admits nothing, exactly like a predicate that does not parse.
TEST_F(RowSecuritySql, SelectPolicyWithNoUsingDeniesEveryRow) {
    const nlohmann::json record = {{"name", "no_condition"},
                                   {"database", "app"},
                                   {"table", "docs"},
                                   {"command", "select"}};
    const auto value = zetta::governance::policy_value_from_json(record);
    ASSERT_TRUE(value);
    ASSERT_TRUE(node.create_policy(*value));

    const std::string bo = user_with_roles("bo", {"writer"});
    QueryService as_bo{node, nullptr, bo, true};
    auto r = as_bo.run("SELECT * FROM app.docs;", "");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_TRUE(r.rows.empty()) << nlohmann::json(r.rows).dump();
    // The same caller can remove no row it cannot see.
    const auto del = as_bo.run("DELETE FROM app.docs;", "");
    QueryService local{node, nullptr, "", false};
    EXPECT_EQ(local.run("SELECT * FROM app.docs;", "").rows.size(), 3u) << del.error;

    // Admin still bypasses.
    const std::string boss = user_with_roles("boss", {"admin"});
    QueryService as_boss{node, nullptr, boss, true};
    auto all = as_boss.run("SELECT * FROM app.docs;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    EXPECT_EQ(all.rows.size(), 3u);
}

TEST_F(RowSecuritySql, WritePolicyWithNoConditionRejectsEveryWrite) {
    const nlohmann::json record = {{"name", "no_check"},
                                   {"database", "app"},
                                   {"table", "docs"},
                                   {"command", "insert"}};
    const auto value = zetta::governance::policy_value_from_json(record);
    ASSERT_TRUE(value);
    ASSERT_TRUE(node.create_policy(*value));

    const std::string ada = user_with_roles("ada", {"writer"});
    QueryService as_ada{node, nullptr, ada, true};
    const auto denied =
        as_ada.run("INSERT INTO app.docs (id, owner) VALUES (10, 'ada');", "");
    EXPECT_FALSE(denied.ok());
    QueryService local{node, nullptr, "", false};
    EXPECT_EQ(local.run("SELECT * FROM app.docs;", "").rows.size(), 3u);
}

// A FOR ALL policy carrying only WITH CHECK still governs reads, and with no
// USING it shows nothing; its writes are decided by the check alone.
TEST_F(RowSecuritySql, AllPolicyWithOnlyCheckDeniesReads) {
    const nlohmann::json record = {{"name", "check_only"},
                                   {"database", "app"},
                                   {"table", "docs"},
                                   {"command", "all"},
                                   {"check", owner_is_current_user()}};
    const auto value = zetta::governance::policy_value_from_json(record);
    ASSERT_TRUE(value);
    ASSERT_TRUE(node.create_policy(*value));

    const std::string ada = user_with_roles("ada", {"writer"});
    QueryService as_ada{node, nullptr, ada, true};
    auto r = as_ada.run("SELECT * FROM app.docs;", "");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_TRUE(r.rows.empty()) << nlohmann::json(r.rows).dump();
    EXPECT_TRUE(
        as_ada.run("INSERT INTO app.docs (id, owner) VALUES (10, 'ada');", "").ok());
    EXPECT_FALSE(
        as_ada.run("INSERT INTO app.docs (id, owner) VALUES (11, 'bo');", "").ok());
}

// A role-scoped policy applies only to the roles it names; a caller with read
// rights but no applicable policy on a protected table sees NOTHING (deny-by-default).
TEST_F(RowSecuritySql, RoleScopedPolicyAppliesOnlyToThatRole) {
    create_policy("CREATE ROLE analyst;");
    // writers see everything; analysts see only their own rows.
    create_policy(
        "CREATE POLICY writers_all ON app.docs FOR SELECT "
        "TO ROLE writer USING (true);");
    create_policy(
        "CREATE POLICY analyst_own ON app.docs FOR SELECT "
        "TO ROLE analyst USING (owner = current_user());");

    // A writer: writers_all (USING true) applies -> every row.
    const std::string wanda = user_with_roles("wanda", {"writer"});
    auto w = QueryService{node, nullptr, wanda, true}.run(
        "SELECT * FROM app.docs;", "");
    ASSERT_TRUE(w.ok()) << w.error;
    EXPECT_EQ(w.rows.size(), 3u);

    // 'ada' (reader gives flat sql:read; analyst makes analyst_own apply) -> own rows.
    const std::string ada = user_with_roles("ada", {"reader", "analyst"});
    auto a = QueryService{node, nullptr, ada, true}.run(
        "SELECT * FROM app.docs;", "");
    ASSERT_TRUE(a.ok()) << a.error;
    ASSERT_EQ(a.rows.size(), 2u);
    for (const auto& row : a.rows) EXPECT_EQ(row["owner"], "ada");

    // A reader with neither writer nor analyst: RLS is engaged (policies exist for
    // SELECT) but none applies -> deny-by-default -> no rows.
    const std::string carol = user_with_roles("carol", {"reader"});
    auto c = QueryService{node, nullptr, carol, true}.run(
        "SELECT * FROM app.docs;", "");
    ASSERT_TRUE(c.ok()) << c.error;
    EXPECT_EQ(c.rows.size(), 0u);
}

// current_roles() wires the caller's role set into a predicate: a member of the
// named role sees all rows; a non-member sees none (that being the only policy).
TEST_F(RowSecuritySql, CurrentRolesMembershipPredicate) {
    create_policy("CREATE ROLE auditor;");
    create_policy(
        "CREATE POLICY auditors_all ON app.docs FOR SELECT "
        "USING ('auditor' = ANY(current_roles()));");

    const std::string ivy = user_with_roles("ivy", {"reader", "auditor"});
    auto seen = QueryService{node, nullptr, ivy, true}.run(
        "SELECT * FROM app.docs;", "");
    ASSERT_TRUE(seen.ok()) << seen.error;
    EXPECT_EQ(seen.rows.size(), 3u);  // an auditor sees all

    const std::string ned = user_with_roles("ned", {"reader"});
    auto none = QueryService{node, nullptr, ned, true}.run(
        "SELECT * FROM app.docs;", "");
    ASSERT_TRUE(none.ok()) << none.error;
    EXPECT_EQ(none.rows.size(), 0u);  // not an auditor -> no matching policy
}

// A non-admin can't route around RLS with GROUP BY (the numeric aggregate path
// can't apply a policy) — it is refused rather than allowed to leak.
TEST_F(RowSecuritySql, GroupByOverProtectedTableIsRefusedForNonAdmin) {
    create_policy(
        "CREATE POLICY own_docs ON app.docs FOR SELECT "
        "USING (owner = current_user());");
    // The RLS guard fires before the DDF aggregate path, so the grouping column's
    // type is irrelevant to the refusal; group by the numeric `id` so the admin
    // sub-case below actually reaches (and runs) the aggregate.
    const std::string ada = user_with_roles("ada", {"writer"});
    auto denied = QueryService{node, nullptr, ada, true}.run(
        "SELECT id, COUNT(*) FROM app.docs GROUP BY id;", "");
    EXPECT_FALSE(denied.ok());
    EXPECT_NE(denied.error.find("row-level security"), std::string::npos)
        << denied.error;

    // admin bypasses, so the aggregate runs for it.
    const std::string boss = user_with_roles("boss", {"admin"});
    auto ok = QueryService{node, nullptr, boss, true}.run(
        "SELECT id, COUNT(*) FROM app.docs GROUP BY id;", "");
    EXPECT_TRUE(ok.ok()) << ok.error;
}

// The DDL is gated on roles:manage — an ordinary user cannot CREATE/DROP POLICY.
TEST_F(RowSecuritySql, PolicyDdlRequiresManagePermission) {
    const std::string ada = user_with_roles("ada", {"writer"});  // no *:manage
    auto denied = QueryService{node, nullptr, ada, true}.run(
        "CREATE POLICY own_docs ON app.docs FOR SELECT "
        "USING (owner = current_user());", "");
    EXPECT_FALSE(denied.ok());

    // An admin user may.
    const std::string boss = user_with_roles("boss", {"admin"});
    auto ok = QueryService{node, nullptr, boss, true}.run(
        "CREATE POLICY own_docs ON app.docs FOR SELECT "
        "USING (owner = current_user());", "");
    EXPECT_TRUE(ok.ok()) << ok.error;
}

// A CREATE POLICY naming a role that doesn't exist is rejected (semantic check,
// not a parse throw) and stores nothing.
TEST_F(RowSecuritySql, PolicyForUnknownRoleIsRejected) {
    auto denied = QueryService{node, nullptr, "", false}.run(
        "CREATE POLICY p ON app.docs FOR SELECT TO ROLE ghost "
        "USING (owner = current_user());", "");
    EXPECT_FALSE(denied.ok());
    EXPECT_EQ(node.policy_count(), 0u);
}

// A CREATE POLICY on a non-existent table is rejected before it is stored.
TEST_F(RowSecuritySql, PolicyOnUnknownTableIsRejected) {
    auto denied = QueryService{node, nullptr, "", false}.run(
        "CREATE POLICY p ON app.ghost FOR SELECT USING (owner = current_user());",
        "");
    EXPECT_FALSE(denied.ok());
    EXPECT_EQ(node.policy_count(), 0u);
}

// system.policies surfaces the registered policies (rendered, no secret material).
TEST_F(RowSecuritySql, SystemPoliciesViewListsThem) {
    create_policy(
        "CREATE POLICY own_docs ON app.docs FOR SELECT "
        "USING (owner = current_user());");
    QueryService local{node, nullptr, "", false};
    auto r = local.run("SELECT * FROM system.policies;", "");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0]["name"], "own_docs");
    EXPECT_EQ(r.rows[0]["table"], "app.docs");
    EXPECT_EQ(r.rows[0]["command"], "select");
    EXPECT_NE(r.rows[0]["using"].get<std::string>().find("current_user()"),
              std::string::npos);
}

// ===== replication ==========================================================

// A policy created on node A converges to node B through the CRDT document.
TEST(RowSecurityReplication, PolicyReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a{zetta::util::new_uuid()};
    ReplicationManager repl_b{zetta::util::new_uuid()};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    ASSERT_TRUE(a.start());
    ASSERT_TRUE(b.start());
    a.ensure_builtin_roles();

    QueryService local{a, nullptr, "", false};
    ASSERT_TRUE(local.run("CREATE DATABASE app;", "").ok());
    ASSERT_TRUE(local.run("CREATE TABLE app.docs (id INT);", "").ok());
    ASSERT_TRUE(
        local.run("CREATE POLICY own_docs ON app.docs FOR SELECT "
                  "USING (owner = current_user());", "").ok());
    a.tick();

    // Ship A's document to B and let B reconcile it on the next tick.
    repl_b.merge(repl_a.save());
    b.tick();

    auto replicated = b.find_policy("own_docs");
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->value().database, "app");
    EXPECT_EQ(replicated->value().table, "docs");
    EXPECT_EQ(replicated->value().command, "select");
    EXPECT_FALSE(replicated->value().using_predicate.is_null());

    a.stop();
    b.stop();
}
