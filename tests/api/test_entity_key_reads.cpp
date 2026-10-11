// #1202: Run and Model reads leave a text entity-key mapping's entries out by
// default. The entries list every distinct id of the dataset, so on 1,500 rows
// get_run answered 222 KB and get_model 192 KB over MCP; a read now carries
// {column, entry_count}, and include_entity_keys returns the entries.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

constexpr std::size_t kRows = 1500;
constexpr std::size_t kAnswerBudget = 20 * 1024;

nlohmann::json text_id_mapping() {
    std::vector<nlohmann::json> ids;
    for (std::size_t row = 0; row < kRows; ++row) {
        ids.emplace_back("customer-" + std::to_string(100000 + row));
    }
    const zetta::data::EntityKeyMappingResult mapped =
        zetta::data::derive_entity_key_mapping("customer_id", ids);
    EXPECT_TRUE(mapped.mapping) << mapped.error;
    return mapped.mapping->to_json();
}

void expect_valid(const nlohmann::json& schema, const nlohmann::json& value) {
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    EXPECT_NO_THROW(validator.validate(value)) << value.dump().substr(0, 400);
}

}  // namespace

TEST(EntityKeyReads, RunAndModelReadsCarryACountUnlessAskedForTheEntries) {
    zetta::test::TempDir dir;
    zetta::NodeConfig config;
    config.data_dir = dir.path().string();
    zetta::Node node(config);
    ASSERT_TRUE(node.start());

    const nlohmann::json mapping = text_id_mapping();
    ASSERT_GT(mapping.dump().size(), kAnswerBudget) << "the fixture must be big enough to matter";

    const auto run = node.create_run();
    ASSERT_TRUE(run);
    std::string error;
    ASSERT_TRUE(run->set_entity_key_mapping(mapping, &error)) << error;

    zetta::ml::ModelValue value;
    value.name = "text-id-model";
    value.kind = "classifier";
    value.run = run->id();
    value.run_id = run->id();
    value.feature_schema = {{"features", {"logins_30d"}}, {"entity_key_mapping", mapping}};
    const auto model = node.create_model(std::move(value));
    ASSERT_TRUE(model);

    Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    const auto call = [&](const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    };
    const nlohmann::json summary = {{"column", "customer_id"}, {"entry_count", kRows}};

    const ApiResult got_run = call("get_run", {{"run_id", run->id()}});
    ASSERT_TRUE(got_run.ok) << got_run.error.message;
    EXPECT_EQ(got_run.value.at("entity_key_mapping"), summary);
    EXPECT_LT(got_run.value.dump().size(), kAnswerBudget);
    expect_valid(registry.find("get_run")->output_schema, got_run.value);

    const ApiResult got_model = call("get_model", {{"model_id", model->id()}});
    ASSERT_TRUE(got_model.ok) << got_model.error.message;
    EXPECT_EQ(got_model.value.at("feature_schema").at("entity_key_mapping"), summary);
    EXPECT_EQ(got_model.value.at("feature_schema").at("features"), nlohmann::json{"logins_30d"});
    EXPECT_LT(got_model.value.dump().size(), kAnswerBudget);
    expect_valid(registry.find("get_model")->output_schema, got_model.value);

    const ApiResult listed = call("list_models", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_EQ(listed.value.at("models")[0].at("feature_schema").at("entity_key_mapping"), summary);
    EXPECT_LT(listed.value.dump().size(), kAnswerBudget);

    // The entries on request, unchanged from the stored record.
    const ApiResult full_run =
        call("get_run", {{"run_id", run->id()}, {"include_entity_keys", true}});
    ASSERT_TRUE(full_run.ok) << full_run.error.message;
    EXPECT_EQ(full_run.value.at("entity_key_mapping"), mapping);
    expect_valid(registry.find("get_run")->output_schema, full_run.value);
    const ApiResult full_model =
        call("get_model", {{"model_id", model->id()}, {"include_entity_keys", true}});
    ASSERT_TRUE(full_model.ok) << full_model.error.message;
    EXPECT_EQ(full_model.value.at("feature_schema").at("entity_key_mapping"), mapping);

    // The stored records still carry every entry: scoring restores ids from them.
    EXPECT_EQ(run->entity_key_mapping(), mapping);
    EXPECT_EQ(model->feature_schema().at("entity_key_mapping"), mapping);
    node.stop();
}

TEST(EntityKeyReads, SummarizingFindsNestedMappingsAndLeavesOthersAlone) {
    nlohmann::json document = {
        {"a", {{"entity_key_mapping", {{"column", "id"}, {"entries", {{{"derived_id", 1}, {"original_value", "x"}}}}}}}},
        {"b", {{{"entity_key_mapping", {{"column", "id"}, {"entry_count", 3}}}}}},
        {"c", "untouched"}};
    EXPECT_EQ(zetta::data::summarize_entity_key_mappings(document), 1u);
    EXPECT_EQ(document["a"]["entity_key_mapping"],
              (nlohmann::json{{"column", "id"}, {"entry_count", 1}}));
    EXPECT_EQ(document["b"][0]["entity_key_mapping"]["entry_count"], 3);
    EXPECT_EQ(document["c"], "untouched");
}
