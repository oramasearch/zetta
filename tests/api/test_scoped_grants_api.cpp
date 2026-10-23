// The API-surface half of fine-grained scoped grants (#747): the per-tool
// `mcp:call` scope enforced by Registry::call (deny -> unauthorized, no side
// effect) and the tools/list filter predicate, plus the blob serve path's
// `blob:read` prefix enforcement (serve_blob). The MCP `tools/call` and the REST
// /v1/<capability> both flow through Registry::call, so this one choke point
// governs both.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;
using zetta::api::ServeRequest;
using zetta::governance::Resource;

struct ScopedApiFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    // Call a capability AS a token-resolved user (the #745 gate identity).
    ApiResult call_as_user(const std::string& user_id, const std::string& name,
                           nlohmann::json input) {
        ApiContext context{node, nullptr, user_id, trace, &node.span_recorder()};
        context.acting_is_user = true;
        return registry.call(name, input, context);
    }

    std::string user_scoped_to_tool(const std::string& tool) {
        node.create_role("toolrole");
        EXPECT_TRUE(node.grant_scoped_permissions("toolrole", {"mcp:call"},
                                                  Resource::tool(tool)));
        auto user = node.create_user("tina");
        node.assign_role(user->id(), node.find_role("toolrole")->id());
        return user->id();
    }
};

// A user with `mcp:call ON TOOL server_info` may call server_info but NOT another
// tool — deny is a clean unauthorized with no side effect.
TEST_F(ScopedApiFixture, PerToolScopeGatesToolsCall) {
    const std::string uid = user_scoped_to_tool("server_info");

    auto allowed = call_as_user(uid, "server_info", nlohmann::json::object());
    EXPECT_TRUE(allowed.ok) << allowed.error.message;

    auto denied = call_as_user(uid, "list_projects", nlohmann::json::object());
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.error.code, "unauthorized");
    EXPECT_NE(denied.error.message.find("mcp:call"), std::string::npos);
    // The denial is audited as not-allowed.
    EXPECT_FALSE(node.audit_events().back()->value().allowed);
}

// A flat mcp:call (the built-in reader) can call every tool — object scoping
// only narrows a role that scopes mcp:call to a subset.
TEST_F(ScopedApiFixture, FlatMcpCallCallsEveryTool) {
    auto reader = node.create_user("rhea");
    node.assign_role(reader->id(), node.find_role("reader")->id());
    EXPECT_TRUE(call_as_user(reader->id(), "server_info",
                             nlohmann::json::object())
                    .ok);
    EXPECT_TRUE(call_as_user(reader->id(), "list_projects",
                             nlohmann::json::object())
                    .ok);
}

// #1185: list_projects' graph summaries are part of get_project's answer, so they
// are read with get_project's right. A user scoped to list_projects alone gets
// the names, as before, and no summaries; one who may also call get_project, or
// holds a flat mcp:call, gets them.
TEST_F(ScopedApiFixture, ListProjectsSummariesNeedGetProjectsRight) {
    ASSERT_TRUE(node.create_project("churn"));
    const std::string uid = user_scoped_to_tool("list_projects");

    auto names_only = call_as_user(uid, "list_projects", nlohmann::json::object());
    ASSERT_TRUE(names_only.ok) << names_only.error.message;
    EXPECT_EQ(names_only.value.at("projects"), nlohmann::json::array({"churn"}));
    EXPECT_FALSE(names_only.value.contains("summaries"));
    EXPECT_FALSE(call_as_user(uid, "get_project", {{"name", "churn"}}).ok);

    ASSERT_TRUE(node.grant_scoped_permissions("toolrole", {"mcp:call"},
                                              Resource::tool("get_project")));
    auto with_detail = call_as_user(uid, "list_projects", nlohmann::json::object());
    ASSERT_TRUE(with_detail.ok) << with_detail.error.message;
    ASSERT_EQ(with_detail.value.at("summaries").size(), 1u);
    EXPECT_EQ(with_detail.value.at("summaries")[0].at("name"), "churn");

    auto reader = node.create_user("rhea");
    node.assign_role(reader->id(), node.find_role("reader")->id());
    auto flat = call_as_user(reader->id(), "list_projects", nlohmann::json::object());
    ASSERT_TRUE(flat.ok) << flat.error.message;
    EXPECT_EQ(flat.value.at("summaries").size(), 1u);
}

// The tools/list filter (http_server dispatch) shows only the tools the caller
// may invoke — exactly the predicate node.may_call_tool over the registry list.
TEST_F(ScopedApiFixture, ToolsListFilterHidesUncallableTools) {
    const std::string uid = user_scoped_to_tool("server_info");
    std::vector<std::string> visible;
    for (const auto& capability : registry.list()) {
        if (node.may_call_tool(uid, /*acting_is_user=*/true, capability->name)) {
            visible.push_back(capability->name);
        }
    }
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible.front(), "server_info");
}

// The blob serve path enforces `blob:read` scoped by prefix: a covered id serves,
// an uncovered id is unauthorized, and admin (flat blob:read) reads either.
TEST_F(ScopedApiFixture, ServeBlobEnforcesPrefixGrant) {
    ASSERT_TRUE(node.create_database("archive"));
    auto db = node.find_database("archive");
    ASSERT_TRUE(db);
    auto object = db->large_objects().import_bytes("payload", "hello blob");
    ASSERT_TRUE(object);
    const std::string blob_id = object->id;

    // A role scoped to exactly this blob's id (a full-id prefix).
    node.create_role("blobrole");
    ASSERT_TRUE(node.grant_scoped_permissions("blobrole", {"blob:read"},
                                              Resource::blob(blob_id)));
    auto user = node.create_user("bella");
    node.assign_role(user->id(), node.find_role("blobrole")->id());

    ServeRequest ok_request;
    ok_request.target = blob_id;
    ok_request.acting_key = user->id();
    ok_request.acting_is_user = true;
    ok_request.trace = trace;
    auto served = zetta::api::serve_blob(node, ok_request);
    ASSERT_TRUE(served.ok) << served.error.message;
    EXPECT_EQ(served.value.at("id"), blob_id);
    EXPECT_EQ(served.value.at("size"), std::string("hello blob").size());

    // A user whose grant does not cover a different id is refused.
    ServeRequest bad_request;
    bad_request.target = "not-my-blob";
    bad_request.acting_key = user->id();
    bad_request.acting_is_user = true;
    bad_request.trace = trace;
    auto refused = zetta::api::serve_blob(node, bad_request);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "unauthorized");
}

}  // namespace
