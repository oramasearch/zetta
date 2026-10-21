// The text entity-key table (#1255, docs/data/entity-key-table.md): a Run or
// Model references its derived-id map, stored as rows of the engine-internal
// `_entity_keys` record collection, instead of carrying every id in the
// replicated record. Records written before it keep their inline map and still
// load.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/entity_key_table.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::cluster::ReplicationManager;
using zetta::data::EntityKeyMapping;
using zetta::data::RecordStore;
using zetta::test::TempDir;

const std::string kTable{zetta::data::kEntityKeyCollection};

// The CRDT key scheme lives in src/core/catalog.hpp, off the tests' include
// path; mirror the two keys this file needs.
std::string run_key(const std::string& id) { return "run:" + id; }
std::string model_key(const std::string& id) { return "model:" + id; }

std::shared_ptr<const EntityKeyMapping> text_ids(std::size_t count,
                                                 const std::string& prefix = "customer-") {
    std::vector<nlohmann::json> ids;
    for (std::size_t row = 0; row < count; ++row) {
        ids.emplace_back(prefix + std::to_string(100000 + row));
    }
    const zetta::data::EntityKeyMappingResult mapped =
        zetta::data::derive_entity_key_mapping("customer_id", ids);
    EXPECT_TRUE(mapped.mapping) << mapped.error;
    return mapped.mapping;
}

std::size_t rows_of(const RecordStore& store, const std::string& owner) {
    std::size_t rows = 0;
    store.scan(kTable, owner + ":", [&](const std::string& key, const std::string&) {
        if (!key.starts_with(owner + ":")) return false;
        ++rows;
        return true;
    });
    return rows;
}

void expect_valid(const nlohmann::json& schema, const nlohmann::json& value) {
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    EXPECT_NO_THROW(validator.validate(value)) << value.dump().substr(0, 400);
}

struct ReplicatedNode {
    explicit ReplicatedNode(const std::filesystem::path& dir, std::string id = zetta::util::new_uuid())
        : replication(std::move(id)),
          node(zetta::NodeConfig{dir.string(), /*preflight=*/false}, &replication) {
        EXPECT_TRUE(node.start());
    }
    ~ReplicatedNode() { node.stop(); }

    zetta::api::ApiResult call(const std::string& name, nlohmann::json input) {
        if (!registry) {
            registry = std::make_unique<zetta::api::Registry>();
            zetta::api::register_core_capabilities(*registry);
        }
        zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
        zetta::api::ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry->call(name, input, context);
    }

    ReplicationManager replication;
    zetta::Node node;
    std::unique_ptr<zetta::api::Registry> registry;
};

}  // namespace

// The rows round-trip: 2,500 ids make three rows of at most 1,024, the
// reference names them, and reading it back gives the same mapping.
TEST(EntityKeyTable, StoresTheMappingAsRowsAndReadsItBack) {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    RecordStore store{replication, replication.node_id(), dir.path() / "records"};
    ASSERT_TRUE(store.open());
    const auto mapping = text_ids(2'500);

    std::string error;
    const auto reference =
        zetta::data::store_entity_key_table(store, "model:m-1", *mapping, &error);
    ASSERT_TRUE(reference) << error;
    EXPECT_EQ(*reference, (nlohmann::json{{"column", "customer_id"},
                                          {"entry_count", 2'500},
                                          {"table", "_entity_keys"},
                                          {"owner", "model:m-1"},
                                          {"chunks", 3}}));
    expect_valid(zetta::data::entity_key_reference_schema(), *reference);
    EXPECT_TRUE(zetta::data::is_entity_key_reference(*reference));
    EXPECT_FALSE(zetta::data::is_entity_key_reference(mapping->to_json()));
    EXPECT_EQ(rows_of(store, "model:m-1"), 3u);
    // The reference is what a record carries: a few dozen bytes, not 2,500 ids.
    EXPECT_LT(reference->dump().size(), 160u);
    EXPECT_GT(mapping->to_json().dump().size(), 100'000u);

    const auto loaded = zetta::data::load_entity_key_mapping(&store, *reference, &error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->to_json(), mapping->to_json());
    EXPECT_EQ(loaded->derived("customer-101234"), mapping->derived("customer-101234"));

    // The rows survive a restart of the store.
    RecordStore reopened{replication, replication.node_id(), dir.path() / "records"};
    ASSERT_TRUE(reopened.open());
    const auto after_restart = zetta::data::load_entity_key_mapping(&reopened, *reference, &error);
    ASSERT_TRUE(after_restart) << error;
    EXPECT_EQ(after_restart->to_json(), mapping->to_json());
}

// An inline mapping (a record written before #1255) reads as it always did,
// with or without a record store.
TEST(EntityKeyTable, AnInlineMappingStillLoads) {
    const auto mapping = text_ids(40);
    std::string error;
    const auto without_store =
        zetta::data::load_entity_key_mapping(nullptr, mapping->to_json(), &error);
    ASSERT_TRUE(without_store) << error;
    EXPECT_EQ(without_store->to_json(), mapping->to_json());

    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    RecordStore store{replication, replication.node_id(), dir.path() / "records"};
    ASSERT_TRUE(store.open());
    const auto with_store = zetta::data::load_entity_key_mapping(&store, mapping->to_json(), &error);
    ASSERT_TRUE(with_store) << error;
    EXPECT_EQ(with_store->to_json(), mapping->to_json());
}

// A node can hold the record before the rows reach it. The reader fails and says
// what is missing; it never restores ids from part of the table.
TEST(EntityKeyTable, AReaderMissingRowsFailsWithAReason) {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    RecordStore store{replication, replication.node_id(), dir.path() / "records"};
    ASSERT_TRUE(store.open());
    std::string error;
    const auto reference =
        zetta::data::store_entity_key_table(store, "run:r-1", *text_ids(2'100), &error);
    ASSERT_TRUE(reference) << error;
    ASSERT_TRUE(store.remove_internal(kTable, "run:r-1:000002"));

    EXPECT_FALSE(zetta::data::load_entity_key_mapping(&store, *reference, &error));
    EXPECT_NE(error.find("2 of the 3 rows"), std::string::npos) << error;
    EXPECT_NE(error.find("have not reached it yet"), std::string::npos) << error;

    TempDir empty_dir;
    RecordStore empty{replication, replication.node_id(), empty_dir.path() / "records"};
    ASSERT_TRUE(empty.open());
    EXPECT_FALSE(zetta::data::load_entity_key_mapping(&empty, *reference, &error));
    EXPECT_NE(error.find("0 of the 3 rows"), std::string::npos) << error;

    EXPECT_FALSE(zetta::data::load_entity_key_mapping(nullptr, *reference, &error));
    EXPECT_NE(error.find("no record store"), std::string::npos) << error;

    nlohmann::json malformed = *reference;
    malformed.erase("owner");
    EXPECT_FALSE(zetta::data::load_entity_key_mapping(&store, malformed, &error));
}

// Rewriting an owner's mapping replaces its rows (a smaller one leaves none
// behind); dropping removes only the rows of the owner the reference names.
TEST(EntityKeyTable, RewritingOrDroppingTouchesOnlyTheOwnersRows) {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    RecordStore store{replication, replication.node_id(), dir.path() / "records"};
    ASSERT_TRUE(store.open());
    std::string error;
    const auto first = zetta::data::store_entity_key_table(store, "model:a", *text_ids(3'000), &error);
    ASSERT_TRUE(first) << error;
    const auto other = zetta::data::store_entity_key_table(store, "model:b", *text_ids(10), &error);
    ASSERT_TRUE(other) << error;
    EXPECT_EQ(rows_of(store, "model:a"), 3u);

    const auto smaller = text_ids(5, "account-");
    const auto rewritten = zetta::data::store_entity_key_table(store, "model:a", *smaller, &error);
    ASSERT_TRUE(rewritten) << error;
    EXPECT_EQ(rows_of(store, "model:a"), 1u);
    EXPECT_EQ(zetta::data::load_entity_key_mapping(&store, *rewritten)->to_json(),
              smaller->to_json());

    // A reference naming model:b is not model:a's to drop.
    zetta::data::drop_entity_key_table(store, *other, "model:a");
    EXPECT_EQ(rows_of(store, "model:b"), 1u);
    zetta::data::drop_entity_key_table(store, *rewritten, "model:a");
    EXPECT_EQ(rows_of(store, "model:a"), 0u);
    EXPECT_EQ(rows_of(store, "model:b"), 1u);
}

// Only the run or model that references them reaches the rows: the record
// capabilities refuse the engine-internal collection, for the owner too.
TEST(EntityKeyTable, TheRecordCapabilitiesCannotReachTheTable) {
    TempDir dir;
    ReplicatedNode replicated(dir.path());
    zetta::Node& node = replicated.node;
    ASSERT_NE(node.record_store(), nullptr);
    const auto run = node.create_run();
    ASSERT_TRUE(run);
    ASSERT_TRUE(run->set_entity_key_mapping(text_ids(20)->to_json()));
    const std::string row = "run:" + run->id() + ":000000";
    ASSERT_EQ(rows_of(*node.record_store(), "run:" + run->id()), 1u);

    EXPECT_FALSE(node.record_store()->get(kTable, row).ok());
    zetta::data::RecordListRequest request;
    request.collection = kTable;
    EXPECT_FALSE(node.record_store()->list(request).ok());

    zetta::api::Registry registry;
    zetta::api::register_record_capabilities(registry);
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    zetta::api::ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
    const auto got = registry.call("get_record", {{"collection", kTable}, {"key", row}}, context);
    EXPECT_FALSE(got.ok);
    const auto listed = registry.call("list_records", {{"collection", kTable}}, context);
    EXPECT_FALSE(listed.ok);
}

// On a node with a record store, new runs and models keep a reference, every
// writer path included, and the reads restore the ids from the table.
TEST(EntityKeyTableNode, NewRunsAndModelsReferenceTheTable) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::string run_id;
    std::string model_id;
    const auto mapping = text_ids(1'500);
    {
        ReplicatedNode replicated(dir.path(), node_id);
        zetta::Node& node = replicated.node;
        const auto run = node.create_run();
        ASSERT_TRUE(run);
        run_id = run->id();
        std::string error;
        ASSERT_TRUE(run->set_entity_key_mapping(mapping->to_json(), &error)) << error;
        const nlohmann::json run_reference = run->entity_key_mapping();
        EXPECT_EQ(run_reference.value("owner", ""), "run:" + run_id);
        EXPECT_EQ(run_reference.value("entry_count", 0), 1'500);
        EXPECT_FALSE(run_reference.contains("entries"));
        // The replicated record carries the reference, not the ids.
        const auto replicated_run = replicated.replication.shared_snapshot()->at(run_key(run_id));
        EXPECT_EQ(replicated_run.at("entity_key_mapping"), run_reference);
        EXPECT_LT(replicated_run.dump().size(), 2'048u);

        zetta::ml::ModelValue value;
        value.name = "text-id-model";
        value.kind = "classifier";
        value.run = run_id;
        value.run_id = run_id;
        value.feature_schema = {{"features", {"logins_30d"}},
                                {"entity_key_mapping", mapping->to_json()}};
        const auto model = node.create_model(std::move(value));
        ASSERT_TRUE(model);
        model_id = model->id();
        const nlohmann::json model_reference = model->feature_schema().at("entity_key_mapping");
        EXPECT_EQ(model_reference.value("owner", ""), "model:" + model_id);
        EXPECT_EQ(replicated.replication.shared_snapshot()
                      ->at(model_key(model_id))
                      .at("feature_schema")
                      .at("entity_key_mapping"),
                  model_reference);

        // The churn and rank runners attach the mapping after training.
        nlohmann::json feature_schema = model->feature_schema();
        feature_schema["entity_key_mapping"] = mapping->to_json();
        feature_schema["inputs"] = {"logins_30d"};
        model->set_feature_schema(std::move(feature_schema));
        EXPECT_EQ(model->feature_schema().at("entity_key_mapping"), model_reference);
        EXPECT_EQ(model->feature_schema().at("inputs"), nlohmann::json{"logins_30d"});
        EXPECT_EQ(rows_of(*node.record_store(), "model:" + model_id), 2u);

        // Reads: a count by default, the entries on request, both valid.
        const nlohmann::json summary = {{"column", "customer_id"}, {"entry_count", 1'500}};
        const auto got_run = replicated.call("get_run", {{"run_id", run_id}});
        ASSERT_TRUE(got_run.ok) << got_run.error.message;
        EXPECT_EQ(got_run.value.at("entity_key_mapping"), summary);
        expect_valid(replicated.registry->find("get_run")->output_schema, got_run.value);
        const auto full_run =
            replicated.call("get_run", {{"run_id", run_id}, {"include_entity_keys", true}});
        ASSERT_TRUE(full_run.ok) << full_run.error.message;
        EXPECT_EQ(full_run.value.at("entity_key_mapping"), mapping->to_json());
        expect_valid(replicated.registry->find("get_run")->output_schema, full_run.value);
        const auto got_model = replicated.call("get_model", {{"model_id", model_id}});
        ASSERT_TRUE(got_model.ok) << got_model.error.message;
        EXPECT_EQ(got_model.value.at("feature_schema").at("entity_key_mapping"), summary);
        const auto full_model =
            replicated.call("get_model", {{"model_id", model_id}, {"include_entity_keys", true}});
        ASSERT_TRUE(full_model.ok) << full_model.error.message;
        EXPECT_EQ(full_model.value.at("feature_schema").at("entity_key_mapping"),
                  mapping->to_json());
        const auto listed = replicated.call("list_models", nlohmann::json::object());
        ASSERT_TRUE(listed.ok) << listed.error.message;
        EXPECT_EQ(listed.value.at("models")[0].at("feature_schema").at("entity_key_mapping"),
                  summary);

        // A run that copies another's mapping (a solution's master run) gets
        // its own rows, so deleting the source does not take them.
        const auto master = node.create_run();
        ASSERT_TRUE(master);
        ASSERT_TRUE(master->set_entity_key_mapping(run->entity_key_mapping(), &error)) << error;
        EXPECT_EQ(master->entity_key_mapping().value("owner", ""), "run:" + master->id());
        EXPECT_EQ(rows_of(*node.record_store(), "run:" + master->id()), 2u);
    }

    // After a restart the reference still resolves from the record log.
    ReplicatedNode restarted(dir.path(), node_id);
    zetta::Node& node = restarted.node;
    const auto model = node.find_model(model_id);
    ASSERT_TRUE(model);
    std::string error;
    const auto resolved =
        node.resolve_entity_key_mapping(model->feature_schema().at("entity_key_mapping"), &error);
    ASSERT_TRUE(resolved) << error;
    EXPECT_EQ(resolved->to_json(), mapping->to_json());

    // Deleting the model and the run deletes their rows, and only theirs.
    ASSERT_TRUE(node.drop_model(model_id));
    EXPECT_EQ(rows_of(*node.record_store(), "model:" + model_id), 0u);
    EXPECT_EQ(rows_of(*node.record_store(), "run:" + run_id), 2u);
    ASSERT_TRUE(node.drop_run(run_id));
    EXPECT_EQ(rows_of(*node.record_store(), "run:" + run_id), 0u);
}

// A Run and a Model written before #1255 (the inline map, straight into the
// replicated catalogue as an older node wrote them) keep loading and reading.
TEST(EntityKeyTableNode, RecordsWrittenBeforeTheTableKeepLoading) {
    TempDir dir;
    ReplicatedNode replicated(dir.path());
    zetta::Node& node = replicated.node;
    const auto mapping = text_ids(300);

    const auto run = node.create_run();
    ASSERT_TRUE(run);
    nlohmann::json old_run = run->metadata();
    old_run["entity_key_mapping"] = mapping->to_json();
    replicated.replication.set(run_key(run->id()), old_run);

    zetta::ml::ModelValue value;
    value.name = "old-model";
    value.kind = "classifier";
    value.run_id = run->id();
    const auto model = node.create_model(std::move(value));
    ASSERT_TRUE(model);
    nlohmann::json old_model = model->metadata();
    old_model["feature_schema"] = {{"features", {"logins_30d"}},
                                   {"entity_key_mapping", mapping->to_json()}};
    replicated.replication.set(model_key(model->id()), old_model);
    node.tick();

    ASSERT_EQ(run->entity_key_mapping(), mapping->to_json());
    ASSERT_EQ(model->feature_schema().at("entity_key_mapping"), mapping->to_json());

    std::string error;
    const auto from_run = node.resolve_entity_key_mapping(run->entity_key_mapping(), &error);
    ASSERT_TRUE(from_run) << error;
    EXPECT_EQ(from_run->to_json(), mapping->to_json());
    const auto from_model =
        node.resolve_entity_key_mapping(model->feature_schema().at("entity_key_mapping"), &error);
    ASSERT_TRUE(from_model) << error;
    EXPECT_EQ(from_model->original(*mapping->derived("customer-100007")), "customer-100007");

    const nlohmann::json summary = {{"column", "customer_id"}, {"entry_count", 300}};
    const auto got_run = replicated.call("get_run", {{"run_id", run->id()}});
    ASSERT_TRUE(got_run.ok) << got_run.error.message;
    EXPECT_EQ(got_run.value.at("entity_key_mapping"), summary);
    const auto full_run =
        replicated.call("get_run", {{"run_id", run->id()}, {"include_entity_keys", true}});
    ASSERT_TRUE(full_run.ok) << full_run.error.message;
    EXPECT_EQ(full_run.value.at("entity_key_mapping"), mapping->to_json());
    expect_valid(replicated.registry->find("get_run")->output_schema, full_run.value);
    const auto full_model =
        replicated.call("get_model", {{"model_id", model->id()}, {"include_entity_keys", true}});
    ASSERT_TRUE(full_model.ok) << full_model.error.message;
    EXPECT_EQ(full_model.value.at("feature_schema").at("entity_key_mapping"), mapping->to_json());

    // Deleting an old model leaves the table alone: it has no rows there.
    ASSERT_TRUE(node.drop_model(model->id()));
}

// expand_entity_key_mappings replaces references anywhere in a document and
// leaves inline maps and everything else alone; an unresolvable one fails.
TEST(EntityKeyTable, ExpandingReplacesEveryReference) {
    const auto mapping = text_ids(3);
    const nlohmann::json reference = {{"column", "customer_id"}, {"entry_count", 3},
                                      {"table", "_entity_keys"}, {"owner", "run:x"},
                                      {"chunks", 1}};
    nlohmann::json document = {{"entity_key_mapping", reference},
                               {"nested", {{{"entity_key_mapping", reference}}}},
                               {"inline", {{"entity_key_mapping", mapping->to_json()}}},
                               {"other", "untouched"}};
    const auto resolve = [&](const nlohmann::json&, std::string*) { return mapping; };
    ASSERT_TRUE(zetta::data::expand_entity_key_mappings(document, resolve));
    EXPECT_EQ(document["entity_key_mapping"], mapping->to_json());
    EXPECT_EQ(document["nested"][0]["entity_key_mapping"], mapping->to_json());
    EXPECT_EQ(document["inline"]["entity_key_mapping"], mapping->to_json());
    EXPECT_EQ(document["other"], "untouched");

    nlohmann::json unresolved = {{"entity_key_mapping", reference}};
    std::string error;
    EXPECT_FALSE(zetta::data::expand_entity_key_mappings(
        unresolved,
        [](const nlohmann::json&, std::string* why) {
            *why = "rows missing";
            return std::shared_ptr<const EntityKeyMapping>{};
        },
        &error));
    EXPECT_EQ(error, "rows missing");

    // A summary of a reference is the same count a summary of the entries gives.
    nlohmann::json summarized = {{"entity_key_mapping", reference}};
    EXPECT_EQ(zetta::data::summarize_entity_key_mappings(summarized), 1u);
    EXPECT_EQ(summarized["entity_key_mapping"],
              (nlohmann::json{{"column", "customer_id"}, {"entry_count", 3}}));
}
