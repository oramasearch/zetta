#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/index/in_memory_index_backend.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/index/index_step_executor.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/data/table.hpp"

using zetta::ExecutionResult;
using zetta::GraphExecutor;
using zetta::Index;
using zetta::IndexBackend;
using zetta::IndexQuery;
using zetta::IndexStepExecutor;
using zetta::Node;
using zetta::NodeConfig;
using zetta::RunOutput;
using zetta::RunStatus;
using zetta::Table;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A "main.accounts" table with five rows over the plan/region fields the index
// covers — three plan=pro (across what will be one partition), one plan=free, and
// one row missing `plan` (tolerated). Returns the table handle.
std::shared_ptr<Table> seed_accounts(Node& node) {
    auto db = node.create_database("main");
    EXPECT_TRUE(db);
    auto table = db->create_table("accounts");
    EXPECT_TRUE(table);
    table->add_row({{"plan", "pro"}, {"region", "us"}});
    table->add_row({{"plan", "free"}, {"region", "eu"}});
    table->add_row({{"plan", "pro"}, {"region", "eu"}});
    table->add_row({{"plan", "pro"}, {"region", "us"}});
    table->add_row({{"region", "us"}});  // missing `plan` — skipped for that field
    return table;
}

// Add an IR `index` step carrying the index definition in its attributes.
void add_index_step(ExecutionGraph& graph, nlohmann::json attributes) {
    graph.add_node(StepKind::Index, "build_accounts_index", std::move(attributes));
}

nlohmann::json search_attributes(std::vector<std::string> fields = {"plan",
                                                                     "region"}) {
    return {{"table", "accounts"},
            {"kind", "search"},
            {"backend", "memory_search"},
            {"fields", fields}};
}

// The row ids in `table` whose `field` equals `value` — the ground truth a query
// over the built index must reproduce (the row ids are generated UUIDs, so read
// them back from the store rather than hard-coding them).
std::vector<std::string> rows_matching(const Table& table,
                                       const std::string& field,
                                       const std::string& value) {
    std::vector<std::string> ids;
    const auto& store = table.store();
    for (std::size_t i = 0; i < store.size(); ++i) {
        const auto& doc = store.at(i);
        if (doc.is_object() && doc.contains(field) && doc[field] == value) {
            ids.push_back(store.id_at(i));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

}  // namespace

// The acceptance (#528): an `index` step in a graph builds a real index that is
// then queryable — via the resolved IndexBackend over the table's rows.
TEST(IndexStepExecutor, BuildsARealIndexThatIsQueryable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto table = seed_accounts(node);

    ExecutionGraph graph;
    add_index_step(graph, search_attributes());

    GraphExecutor executor{std::make_shared<IndexStepExecutor>(node)};
    const ExecutionResult result = executor.execute(node, graph);

    // The run succeeded and produced one `index` output referencing a REAL Index.
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.run);
    EXPECT_EQ(result.run->status(), RunStatus::Succeeded);
    const std::vector<RunOutput>& outputs = result.run->outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0].kind, "index");

    // The output ref resolves to a durable Index entity the node recorded.
    std::shared_ptr<Index> index = node.find_index(outputs[0].ref);
    ASSERT_TRUE(index) << "index output ref does not resolve to a real Index";
    EXPECT_EQ(index->table(), "accounts");
    EXPECT_EQ(index->backend(), "memory_search");
    EXPECT_EQ(node.index_count(), 1u);

    // QUERYABLE: the built index answers a query through the node's registry.
    std::shared_ptr<IndexBackend> backend =
        node.index_registry().resolve(index->spec());
    ASSERT_TRUE(backend);

    IndexQuery pro;
    pro.field = "plan";
    pro.term = "pro";
    auto pro_result = backend->query(index->spec(), pro);
    ASSERT_TRUE(pro_result.status.ok()) << pro_result.status.message;
    std::sort(pro_result.row_ids.begin(), pro_result.row_ids.end());
    EXPECT_EQ(pro_result.row_ids, rows_matching(*table, "plan", "pro"));
    EXPECT_EQ(pro_result.row_ids.size(), 3u);

    // A second indexed field works too (region == "eu").
    IndexQuery eu;
    eu.field = "region";
    eu.term = "eu";
    auto eu_result = backend->query(index->spec(), eu);
    ASSERT_TRUE(eu_result.status.ok());
    std::sort(eu_result.row_ids.begin(), eu_result.row_ids.end());
    EXPECT_EQ(eu_result.row_ids, rows_matching(*table, "region", "eu"));

    node.stop();
}

// A connect -> index graph: the index step builds a real index, and the non-index
// step delegates to DefaultStepExecutor (a connect emits a dataset + a source).
TEST(IndexStepExecutor, DelegatesNonIndexStepsAndBuildsTheIndex) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    seed_accounts(node);

    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "ingest",
                                  {{"source", "postgres"}, {"table", "accounts"}});
    auto index = graph.add_node(StepKind::Index, "build_accounts_index",
                                search_attributes());
    graph.add_edge(connect->id, index->id);

    GraphExecutor executor{std::make_shared<IndexStepExecutor>(node)};
    const ExecutionResult result = executor.execute(node, graph);
    ASSERT_TRUE(result.ok) << result.error;

    const std::vector<RunOutput>& outputs = result.run->outputs();
    // connect -> a dataset output; index -> an index output referencing a real Index.
    EXPECT_EQ(std::count_if(outputs.begin(), outputs.end(),
                            [](const RunOutput& o) { return o.kind == "dataset"; }),
              1);
    const auto index_output =
        std::find_if(outputs.begin(), outputs.end(),
                     [](const RunOutput& o) { return o.kind == "index"; });
    ASSERT_NE(index_output, outputs.end());
    EXPECT_TRUE(node.find_index(index_output->ref));
    // The connect step recorded a lineage source.
    EXPECT_FALSE(result.run->lineage().sources.empty());

    node.stop();
}

// An index step whose backend does not resolve fails the run with a clear reason
// (and records nothing) — the boundary, not a silent no-op.
TEST(IndexStepExecutor, FailsOnUnresolvedBackend) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    seed_accounts(node);

    ExecutionGraph graph;
    add_index_step(graph, {{"table", "accounts"},
                           {"kind", "search"},
                           {"backend", "no_such_backend"},
                           {"fields", {"plan"}}});

    GraphExecutor executor{std::make_shared<IndexStepExecutor>(node)};
    const ExecutionResult result = executor.execute(node, graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.run->status(), RunStatus::Failed);
    EXPECT_NE(result.error.find("no_such_backend"), std::string::npos);
    EXPECT_EQ(node.index_count(), 0u);

    node.stop();
}

// An index step naming a table that does not exist fails the run.
TEST(IndexStepExecutor, FailsOnMissingSourceTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    // No table seeded.

    ExecutionGraph graph;
    add_index_step(graph, search_attributes());

    GraphExecutor executor{std::make_shared<IndexStepExecutor>(node)};
    const ExecutionResult result = executor.execute(node, graph);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("not found"), std::string::npos);
    EXPECT_EQ(node.index_count(), 0u);

    node.stop();
}

// An index step whose attributes are not a valid index spec fails the run.
TEST(IndexStepExecutor, FailsOnInvalidSpec) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    seed_accounts(node);

    ExecutionGraph graph;
    // Missing `fields` — index_spec_from_node rejects it.
    add_index_step(graph, {{"table", "accounts"},
                           {"kind", "search"},
                           {"backend", "memory_search"}});

    GraphExecutor executor{std::make_shared<IndexStepExecutor>(node)};
    const ExecutionResult result = executor.execute(node, graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(node.index_count(), 0u);

    node.stop();
}

// The explicit-registry constructor builds into a caller-owned registry (isolation
// for tests / a host-curated backend set).
TEST(IndexStepExecutor, BuildsIntoAnExplicitRegistry) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto table = seed_accounts(node);

    zetta::IndexRegistry registry;
    registry.add(std::make_shared<zetta::InMemoryIndexBackend>());

    ExecutionGraph graph;
    add_index_step(graph, search_attributes({"plan"}));

    GraphExecutor executor{
        std::make_shared<IndexStepExecutor>(node, registry)};
    const ExecutionResult result = executor.execute(node, graph);
    ASSERT_TRUE(result.ok) << result.error;

    auto index = node.find_index(result.run->outputs()[0].ref);
    ASSERT_TRUE(index);
    // The built index lives in the explicit registry, not the node's own.
    auto backend = registry.resolve(index->spec());
    ASSERT_TRUE(backend);
    IndexQuery q;
    q.field = "plan";
    q.term = "pro";
    EXPECT_EQ(backend->query(index->spec(), q).row_ids,
              rows_matching(*table, "plan", "pro"));

    node.stop();
}
