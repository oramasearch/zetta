// SPDX-License-Identifier: MIT

// analyze_dataset and plan_features read AS their caller (#1300, #1396): the same
// table grants, column grants and row security a SELECT of the caller's would get,
// never the trusted local session. A column the caller may not read is omitted; a
// table it may not read answers as a table that does not exist and is never sampled
// for joins; row security keeps other people's rows out of the statistics.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

std::vector<std::string> column_names(const nlohmann::json& analysis) {
    std::vector<std::string> names;
    for (const nlohmann::json& column : analysis.at("columns")) {
        names.push_back(column.at("name").get<std::string>());
    }
    return names;
}

struct AnalysisAccess : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        zetta::api::register_core_capabilities(registry);

        auto sales = node.create_database("sales");
        ASSERT_TRUE(sales);
        auto customers = sales->create_table("customers");
        ASSERT_TRUE(customers);
        customers->set_column_order({"customer_id", "region", "ssn", "churned"});
        // The neighbour shares the customer ids, so a reader of it would see it as
        // joinable; a caller without a grant on it must not.
        auto secrets = sales->create_table("secrets");
        ASSERT_TRUE(secrets);
        for (int index = 0; index < 60; ++index) {
            customers->add_row({{"customer_id", index + 1},
                                {"region", index % 2 == 0 ? "north" : "south"},
                                {"ssn", "ssn-" + std::to_string(1000 + index)},
                                {"churned", index % 3 == 0 ? 1 : 0}});
            secrets->add_row({{"customer_id", index + 1},
                              {"vault_code", "vault-" + std::to_string(index)}});
        }

        // A clerk reads customer_id, region and churned of sales.customers, nothing
        // else: not the ssn column, not the secrets table.
        ASSERT_TRUE(node.create_role("clerk"));
        ASSERT_TRUE(node.grant_permissions("clerk", {"api:query", "mcp:call"}));
        ASSERT_TRUE(node.grant_scoped_permissions(
            "clerk", {"sql:read"},
            zetta::governance::Resource::column("sales", "customers",
                                                {"customer_id", "region", "churned"})));
        const auto user = node.create_user("cleo");
        ASSERT_TRUE(user);
        ASSERT_TRUE(node.assign_role(user->id(), node.find_role("clerk")->id()));
        clerk = user->id();
    }
    void TearDown() override { node.stop(); }

    ApiResult call_as(const std::string& acting_key, const std::string& name,
                      const nlohmann::json& input) {
        ApiContext context{node, nullptr, acting_key, trace, &node.span_recorder()};
        context.acting_is_user = !acting_key.empty();
        return registry.call(name, input, context);
    }

    std::string clerk;
};

// #1300: a column the caller may not read is omitted, never described, and the
// readable columns' statistics are the ones a full-access caller gets.
TEST_F(AnalysisAccess, AnalyzeDatasetOmitsColumnsTheCallerMayNotRead) {
    const ApiResult scoped =
        call_as(clerk, "analyze_dataset", {{"dataset_path", "sales.customers"}});
    ASSERT_TRUE(scoped.ok) << scoped.error.message;
    EXPECT_EQ(column_names(scoped.value),
              (std::vector<std::string>{"customer_id", "region", "churned"}));
    EXPECT_EQ(scoped.value.dump().find("ssn"), std::string::npos)
        << "the hidden column is not named, counted or sampled: " << scoped.value.dump();

    const ApiResult whole = call_as("", "analyze_dataset", {{"dataset_path", "sales.customers"}});
    ASSERT_TRUE(whole.ok) << whole.error.message;
    EXPECT_EQ(column_names(whole.value),
              (std::vector<std::string>{"customer_id", "region", "ssn", "churned"}));
    for (const nlohmann::json& column : scoped.value.at("columns")) {
        for (const nlohmann::json& full : whole.value.at("columns")) {
            if (full.at("name") == column.at("name")) {
                EXPECT_EQ(column, full) << "the readable columns' sample is unchanged";
            }
        }
    }
}

// #1396: the planner sees only what the caller may read.
TEST_F(AnalysisAccess, PlanFeaturesNeverPlansAColumnTheCallerMayNotRead) {
    const ApiResult plan = call_as(clerk, "plan_features",
                                   {{"dataset_path", "sales.customers"},
                                    {"methodology", "M-1"},
                                    {"label", "churned"}});
    ASSERT_TRUE(plan.ok) << plan.error.message;
    EXPECT_EQ(plan.value.dump().find("ssn"), std::string::npos) << plan.value.dump();
}

// #1396: a table the caller holds no read on answers as a missing table, by either
// spelling, and is never sampled as a join neighbour.
TEST_F(AnalysisAccess, ATableTheCallerMayNotReadAnswersAsMissingAndIsNeverJoined) {
    const ApiResult refused =
        call_as(clerk, "analyze_dataset", {{"dataset_path", "sales.secrets"}});
    ASSERT_FALSE(refused.ok);
    const ApiResult missing =
        call_as(clerk, "analyze_dataset", {{"dataset_path", "sales.nothing"}});
    ASSERT_FALSE(missing.ok);
    EXPECT_EQ(refused.error.code, missing.error.code);
    EXPECT_EQ(refused.error.message, "dataset: no table 'sales.secrets'");
    EXPECT_EQ(missing.error.message, "dataset: no table 'sales.nothing'");

    const ApiResult bare = call_as(clerk, "analyze_dataset", {{"dataset", "secrets"}});
    ASSERT_FALSE(bare.ok);
    EXPECT_EQ(bare.error.message, "dataset: no dataset named 'secrets'");

    const ApiResult scoped =
        call_as(clerk, "analyze_dataset", {{"dataset_path", "sales.customers"}});
    ASSERT_TRUE(scoped.ok) << scoped.error.message;
    EXPECT_TRUE(scoped.value.at("joinable_tables").empty()) << scoped.value.dump();
    EXPECT_EQ(scoped.value.dump().find("vault"), std::string::npos);

    // The trusted local session still finds the neighbour.
    const ApiResult whole = call_as("", "analyze_dataset", {{"dataset_path", "sales.customers"}});
    ASSERT_TRUE(whole.ok) << whole.error.message;
    EXPECT_NE(whole.value.at("joinable_tables").dump().find("secrets"), std::string::npos)
        << whole.value.dump();
}

// #1396: row security applies to the sample, so other people's rows never reach the
// statistics.
TEST_F(AnalysisAccess, RowSecurityKeepsOtherRowsOutOfTheAnalysis) {
    auto app = node.create_database("app");
    ASSERT_TRUE(app);
    auto docs = app->create_table("docs");
    ASSERT_TRUE(docs);
    for (int index = 0; index < 20; ++index) {
        docs->add_row({{"id", index}, {"owner", "ada"}, {"code", "ada-" + std::to_string(index % 3)}});
        docs->add_row({{"id", 100 + index}, {"owner", "bo"}, {"code", "bo-only-" + std::to_string(index % 3)}});
    }
    {
        ApiContext local{node, nullptr, "", trace, &node.span_recorder()};
        const ApiResult policy = registry.call(
            "sql",
            {{"statement", "CREATE POLICY own_docs ON app.docs FOR SELECT "
                           "USING (owner = current_user());"},
             {"database", ""}},
            local);
        ASSERT_TRUE(policy.ok) << policy.error.message;
    }
    const auto ada = node.create_user("ada");
    ASSERT_TRUE(ada);
    ASSERT_TRUE(node.assign_role(ada->id(), node.find_role("reader")->id()));

    const ApiResult own = call_as(ada->id(), "analyze_dataset", {{"dataset_path", "app.docs"}});
    ASSERT_TRUE(own.ok) << own.error.message;
    EXPECT_EQ(own.value.at("sampled_rows"), 20);
    EXPECT_EQ(own.value.dump().find("bo-only"), std::string::npos) << own.value.dump();
    EXPECT_EQ(own.value.dump().find("\"bo\""), std::string::npos) << own.value.dump();

    const ApiResult whole = call_as("", "analyze_dataset", {{"dataset_path", "app.docs"}});
    ASSERT_TRUE(whole.ok) << whole.error.message;
    EXPECT_EQ(whole.value.at("sampled_rows"), 40);
}

}  // namespace
