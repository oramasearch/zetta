// The API-surface half of row-level security (#748): RLS enforced end-to-end
// through the real `sql` capability (the MCP `tools/call` + REST /v1/sql choke
// point), driven AS a token-resolved user (the #745 gate identity). Proves the
// gate -> executor path applies USING filters and the admin bypass exactly as the
// engine-level tests assert directly.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

struct RlsApiFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        zetta::api::register_core_capabilities(registry);
        // Seed + policy as the trusted-local session (empty acting key).
        run_local("CREATE DATABASE app;");
        run_local("CREATE TABLE app.docs (id INT);");
        run_local("INSERT INTO app.docs (id, owner) VALUES (1, 'ada');");
        run_local("INSERT INTO app.docs (id, owner) VALUES (2, 'bo');");
        run_local(
            "CREATE POLICY own_docs ON app.docs FOR SELECT "
            "USING (owner = current_user());");
    }
    void TearDown() override { node.stop(); }

    ApiResult sql_call(const std::string& acting_key, bool is_user,
                       const std::string& statement) {
        ApiContext context{node, nullptr, acting_key, trace,
                           &node.span_recorder()};
        context.acting_is_user = is_user;
        return registry.call(
            "sql",
            nlohmann::json{{"statement", statement}, {"database", ""}}, context);
    }
    void run_local(const std::string& statement) {
        auto r = sql_call("", /*is_user=*/false, statement);
        ASSERT_TRUE(r.ok) << r.error.message << "  (" << statement << ")";
    }

    std::string user_with_role(const std::string& name, const std::string& role) {
        auto user = node.create_user(name);
        EXPECT_TRUE(user);
        auto r = node.find_role(role);
        EXPECT_TRUE(r) << role;
        if (user && r) node.assign_role(user->id(), r->id());
        return user ? user->id() : std::string{};
    }
};

// A non-admin user's SELECT through the `sql` capability comes back row-filtered.
TEST_F(RlsApiFixture, NonAdminSelectIsRowFilteredThroughTheApi) {
    const std::string ada = user_with_role("ada", "writer");
    auto r = sql_call(ada, /*is_user=*/true, "SELECT * FROM app.docs;");
    ASSERT_TRUE(r.ok) << r.error.message;
    const auto& rows = r.value.at("rows");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["owner"], "ada");
}

// An admin user bypasses RLS through the same choke point.
TEST_F(RlsApiFixture, AdminSeesEveryRowThroughTheApi) {
    const std::string boss = user_with_role("boss", "admin");
    auto r = sql_call(boss, /*is_user=*/true, "SELECT * FROM app.docs;");
    ASSERT_TRUE(r.ok) << r.error.message;
    EXPECT_EQ(r.value.at("rows").size(), 2u);
}

}  // namespace
