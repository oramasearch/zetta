// Per-connection grants (#1339): a saved connection's credential is usable
// only by the identity that created it, by a holder of connections:use (flat,
// as admin holds it, or ON CONNECTION <name>), or by the trusted local session.
// Holding the capability that opens connections is not enough: knowing a
// connection's name is not being able to use it. Replacing, deleting or
// changing the safe mode of a connection needs its owner or connections:manage
// on it. Every refusal is audited with the caller and the connection's name.
#include <gtest/gtest.h>

#include <string>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

struct ConnectionGrantFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::api::BackgroundActions background;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    std::string alice;
    std::string bob;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        node.ensure_builtin_roles();
        // Both hold the orchestration rights that open connections.
        const auto setup = call(
            "sql", {{"statement",
                     "CREATE ROLE operators; GRANT pipelines:manage, mcp:call TO ROLE "
                     "operators; CREATE USER alice; GRANT ROLE operators TO alice; "
                     "CREATE USER bob; GRANT ROLE operators TO bob; CREATE ROLE "
                     "bob_extra; GRANT ROLE bob_extra TO bob;"}});
        ASSERT_TRUE(setup.ok) << setup.error.message;
        alice = node.find_user("alice")->id();
        bob = node.find_user("bob")->id();
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input,
                   const std::string& user = {}) {
        ApiContext context{node, nullptr, user, trace, &node.span_recorder(), &background};
        context.acting_is_user = !user.empty();
        return registry.call(name, input, context);
    }

    nlohmann::json connector(const std::string& token) {
        // An unreachable endpoint: a use that passes the grant check fails on
        // the network or the destination policy, never as `unauthorized`.
        return {{"type", "snowflake"},
                {"account", "ab12345"},
                {"token", token},
                {"endpoint", "http://127.0.0.1:1"}};
    }

    std::size_t refusals_for(const std::string& actor, const std::string& connection) {
        std::size_t count = 0;
        for (const auto& event : node.audit_events()) {
            const auto& value = event->value();
            if (value.actor == actor && value.target == "connection:" + connection &&
                !value.allowed) {
                ++count;
            }
        }
        return count;
    }
};

TEST_F(ConnectionGrantFixture, AnotherHolderOfTheRightCannotUseOrReplaceAConnection) {
    ASSERT_TRUE(call("create_connection",
                     {{"name", "alice_wh"}, {"connector", connector("alice-secret")}}, alice)
                    .ok);
    const auto saved = zetta::connect::find_connection(node, "alice_wh");
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->owner, alice);

    // Bob holds every orchestration right that opens a connection, and knows
    // the name: each use is refused and audited.
    const auto listed = call("list_source_tables", {{"connection", "alice_wh"}}, bob);
    EXPECT_EQ(listed.error.code, "unauthorized") << listed.error.message;
    EXPECT_NE(listed.error.message.find("connections:use"), std::string::npos);
    const auto pulled = call("create_dataset_from_query",
                             {{"connection", "alice_wh"}, {"query", "SELECT 1"}}, bob);
    EXPECT_EQ(pulled.error.code, "unauthorized") << pulled.error.message;
    EXPECT_GE(refusals_for(bob, "alice_wh"), 2u);

    // Bob cannot repoint it at his own account, nor delete it.
    const auto replaced = call("create_connection",
                               {{"name", "alice_wh"}, {"connector", connector("bob-secret")}},
                               bob);
    EXPECT_EQ(replaced.error.code, "unauthorized") << replaced.error.message;
    EXPECT_EQ(zetta::connect::find_connection(node, "alice_wh")->config.token, "alice-secret");
    EXPECT_EQ(call("delete_connection", {{"name", "alice_wh"}}, bob).error.code, "unauthorized");
    EXPECT_EQ(call("set_safe_mode", {{"connection", "alice_wh"}, {"mode", "off"}}, bob)
                  .error.code,
              "unauthorized");
    EXPECT_TRUE(zetta::connect::find_connection(node, "alice_wh"));

    // Its creator uses it: the call gets past the grant check.
    EXPECT_NE(call("list_source_tables", {{"connection", "alice_wh"}}, alice).error.code,
              "unauthorized");
    EXPECT_EQ(refusals_for(alice, "alice_wh"), 0u);

    // A grant ON CONNECTION lets Bob use it, and only use it.
    ASSERT_TRUE(call("sql", {{"statement",
                              "GRANT connections:use ON CONNECTION alice_wh TO ROLE bob_extra;"}})
                    .ok);
    EXPECT_NE(call("list_source_tables", {{"connection", "alice_wh"}}, bob).error.code,
              "unauthorized");
    EXPECT_EQ(call("create_connection",
                   {{"name", "alice_wh"}, {"connector", connector("bob-secret")}}, bob)
                  .error.code,
              "unauthorized");
    // connections:manage on it lets him rotate it; Alice stays the owner.
    ASSERT_TRUE(call("sql", {{"statement",
                              "GRANT connections:manage ON CONNECTION alice_wh TO ROLE "
                              "bob_extra;"}})
                    .ok);
    ASSERT_TRUE(call("create_connection",
                     {{"name", "alice_wh"}, {"connector", connector("rotated-secret")}}, bob)
                    .ok);
    const auto rotated = zetta::connect::find_connection(node, "alice_wh");
    ASSERT_TRUE(rotated);
    EXPECT_EQ(rotated->owner, alice);
    EXPECT_EQ(rotated->config.token, "rotated-secret");
}

TEST_F(ConnectionGrantFixture, AdminAndTheLocalSessionMayUseAnyConnection) {
    ASSERT_TRUE(call("create_connection",
                     {{"name", "shared"}, {"connector", connector("shared-secret")}}, alice)
                    .ok);
    const auto admin_user = node.create_user("boss");
    ASSERT_TRUE(admin_user);
    node.assign_role(admin_user->id(), node.find_role("admin")->id());
    EXPECT_NE(call("list_source_tables", {{"connection", "shared"}}, admin_user->id()).error.code,
              "unauthorized");
    EXPECT_NE(call("list_source_tables", {{"connection", "shared"}}).error.code, "unauthorized");

    // A row saved before the owner was recorded (or by the local session) has
    // no owner: only a holder of the right may use it.
    ASSERT_TRUE(call("create_connection",
                     {{"name", "unowned"}, {"connector", connector("local-secret")}})
                    .ok);
    EXPECT_TRUE(zetta::connect::find_connection(node, "unowned")->owner.empty());
    EXPECT_EQ(call("list_source_tables", {{"connection", "unowned"}}, alice).error.code,
              "unauthorized");

    // The grant tier names one connection exactly, never a prefix.
    using zetta::governance::Resource;
    EXPECT_TRUE(zetta::governance::object_covers(Resource::connection("shared"),
                                                 Resource::connection("shared")));
    EXPECT_FALSE(zetta::governance::object_covers(Resource::connection("shared"),
                                                  Resource::connection("shared_two")));
}

}  // namespace
