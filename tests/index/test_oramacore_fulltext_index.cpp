#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/index/oramacore_fulltext_index.hpp"
#include "zetta/ir/execution_graph.hpp"

using zetta::index::IndexDocument;
using zetta::index::IndexKind;
using zetta::index::IndexPartition;
using zetta::index::IndexQuery;
using zetta::index::IndexRegistry;
using zetta::index::IndexSpec;
using zetta::index::IndexStatusCode;
using zetta::index::OramaCoreFulltextIndex;

namespace {

// A small "table" of text rows in two partitions (chunks) — the unit a backend
// builds over. "postgres" appears 4x in r1 (title + 3x body) and 1x in r3, so a
// "postgres" query ranks r1 above r3 unambiguously; "and" is a stop-word.
std::vector<IndexPartition> sample_partitions() {
    IndexPartition chunk0;
    chunk0.id = "000000";
    chunk0.documents = {
        IndexDocument{"r1",
                      {{"title", "Postgres performance"},
                       {"body", "postgres postgres tuning and postgres scaling"}}},
        IndexDocument{"r2",
                      {{"title", "Cooking pasta"},
                       {"body", "boil water then add salt and pasta"}}},
        IndexDocument{"r3",
                      {{"title", "Database indexes"},
                       {"body", "btree gin brin and postgres indexes explained"}}},
    };
    IndexPartition chunk1;
    chunk1.id = "000001";
    chunk1.documents = {
        IndexDocument{"r4",
                      {{"title", "Widget catalog"},
                       {"body", "a guide about widgets and gadgets"}}},
        // A row missing the indexed fields is tolerated (it just never matches).
        IndexDocument{"r5", {{"unindexed", "ignored"}}},
    };
    return {chunk0, chunk1};
}

// The full-text index spec: a Search/full-text index over the title + body fields,
// built by the oramacore_fulltext backend.
IndexSpec fulltext_spec() {
    return IndexSpec{"articles", IndexKind::Search,
                     std::string(OramaCoreFulltextIndex::kName),
                     {"title", "body"}};
}

}  // namespace

// ----- backend identity ------------------------------------------------------

TEST(OramaCoreFulltextIndex, NameAndKind) {
    OramaCoreFulltextIndex backend;
    EXPECT_EQ(backend.name(), "oramacore_fulltext");
    EXPECT_EQ(backend.kind(), IndexKind::Search);
}

// When the private oramacore-cpp submodule was not compiled in, the backend stays
// usable but reports the engine is missing — so a CPU-only checkout links + runs.
TEST(OramaCoreFulltextIndex, DegradesGracefullyWithoutEngine) {
    if (OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "engine compiled in — covered by the build/query tests";
    }
    OramaCoreFulltextIndex backend;
    EXPECT_EQ(backend.build(fulltext_spec(), sample_partitions()).code,
              IndexStatusCode::Unsupported);
    IndexQuery q;
    q.term = "postgres";
    EXPECT_EQ(backend.query(fulltext_spec(), q).status.code,
              IndexStatusCode::Unsupported);
    EXPECT_TRUE(backend.drop(fulltext_spec()).ok());  // nothing built — Ok
}

// ----- build + query (the acceptance: build + query a full-text index) --------

TEST(OramaCoreFulltextIndex, BuildsAndRanksFullTextOverASmallTable) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    OramaCoreFulltextIndex backend;
    const IndexSpec spec = fulltext_spec();

    ASSERT_TRUE(backend.build(spec, sample_partitions()).ok());
    EXPECT_EQ(backend.built_count(), 1u);

    // "postgres" matches r1 (4x) and r3 (1x); r1 ranks first by BM25F relevance.
    IndexQuery q;
    q.term = "postgres";
    auto result = backend.query(spec, q);
    ASSERT_TRUE(result.status.ok()) << result.status.message;
    EXPECT_EQ(result.row_ids, (std::vector<std::string>{"r1", "r3"}));

    // top_k caps the ranked list to the most relevant.
    q.top_k = 1;
    EXPECT_EQ(backend.query(spec, q).row_ids, (std::vector<std::string>{"r1"}));

    // Tokenization matches on both sides: "widgets" stems the same as "widget".
    IndexQuery widget;
    widget.term = "widget";
    EXPECT_EQ(backend.query(spec, widget).row_ids,
              (std::vector<std::string>{"r4"}));

    // A stop-word query yields no tokens -> an empty (successful) result.
    IndexQuery stop;
    stop.term = "and";
    auto stop_result = backend.query(spec, stop);
    EXPECT_TRUE(stop_result.status.ok());
    EXPECT_TRUE(stop_result.row_ids.empty());

    // A no-match query is empty + successful.
    IndexQuery miss;
    miss.term = "kubernetes";
    EXPECT_TRUE(backend.query(spec, miss).status.ok());
    EXPECT_TRUE(backend.query(spec, miss).row_ids.empty());

    // Drop is idempotent.
    EXPECT_TRUE(backend.drop(spec).ok());
    EXPECT_EQ(backend.built_count(), 0u);
    EXPECT_TRUE(backend.drop(spec).ok());
}

TEST(OramaCoreFulltextIndex, RejectsBadSpecsAndQueries) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    OramaCoreFulltextIndex backend;

    // A non-Search spec is unsupported by this backend.
    IndexSpec vector_spec{"articles", IndexKind::Vector,
                          std::string(OramaCoreFulltextIndex::kName), {"body"}};
    EXPECT_EQ(backend.build(vector_spec, {}).code, IndexStatusCode::Unsupported);

    // No fields => invalid.
    IndexSpec no_fields{"articles", IndexKind::Search,
                        std::string(OramaCoreFulltextIndex::kName), {}};
    EXPECT_EQ(backend.build(no_fields, {}).code, IndexStatusCode::InvalidArgument);

    // Query before build => not found.
    const IndexSpec spec = fulltext_spec();
    IndexQuery q;
    q.term = "postgres";
    EXPECT_EQ(backend.query(spec, q).status.code, IndexStatusCode::NotFound);

    ASSERT_TRUE(backend.build(spec, sample_partitions()).ok());

    // A named field the index does not cover => invalid argument.
    IndexQuery bad_field;
    bad_field.field = "not_indexed";
    bad_field.term = "postgres";
    EXPECT_EQ(backend.query(spec, bad_field).status.code,
              IndexStatusCode::InvalidArgument);

    // A non-text (array) query term => invalid argument.
    IndexQuery bad_term;
    bad_term.term = nlohmann::json::array({1, 2});
    EXPECT_EQ(backend.query(spec, bad_term).status.code,
              IndexStatusCode::InvalidArgument);

    // A named field that IS indexed is accepted.
    IndexQuery good_field;
    good_field.field = "title";
    good_field.term = "postgres";
    EXPECT_TRUE(backend.query(spec, good_field).status.ok());
}

// lake#782: a query naming a field searches that field only. A row whose match is
// in another indexed field is not a hit.
TEST(OramaCoreFulltextIndex, ANamedFieldLimitsTheSearchToThatField) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    OramaCoreFulltextIndex backend;
    const IndexSpec spec = fulltext_spec();
    ASSERT_TRUE(backend.build(spec, sample_partitions()).ok());
    const auto search = [&](const std::string& field, const std::string& term) {
        IndexQuery q;
        q.field = field;
        q.term = term;
        const auto result = backend.query(spec, q);
        EXPECT_TRUE(result.status.ok()) << result.status.message;
        EXPECT_EQ(result.row_ids.size(), result.scores.size());
        return result.row_ids;
    };

    // "postgres" is in r1's title and body, and only in r3's body.
    EXPECT_EQ(search("", "postgres"), (std::vector<std::string>{"r1", "r3"}));
    EXPECT_EQ(search("title", "postgres"), (std::vector<std::string>{"r1"}));
    EXPECT_EQ(search("body", "postgres"), (std::vector<std::string>{"r1", "r3"}));

    // "guide" is only in r4's body: every field finds it, the title does not.
    EXPECT_EQ(search("", "guide"), (std::vector<std::string>{"r4"}));
    EXPECT_TRUE(search("title", "guide").empty());
    EXPECT_EQ(search("body", "guide"), (std::vector<std::string>{"r4"}));

    // "catalog" is only in r4's title; the row missing both fields (r5) never matches.
    EXPECT_EQ(search("title", "catalog"), (std::vector<std::string>{"r4"}));
    EXPECT_TRUE(search("body", "catalog").empty());

    // An index over one field answers the same with or without naming it.
    const IndexSpec titles{
        "articles", IndexKind::Search, std::string(OramaCoreFulltextIndex::kName), {"title"}};
    ASSERT_TRUE(backend.build(titles, sample_partitions()).ok());
    IndexQuery named;
    named.field = "title";
    named.term = "postgres";
    IndexQuery unnamed;
    unnamed.term = "postgres";
    EXPECT_EQ(backend.query(titles, named).row_ids, (std::vector<std::string>{"r1"}));
    EXPECT_EQ(backend.query(titles, unnamed).row_ids, (std::vector<std::string>{"r1"}));
}

// ----- end-to-end via the registry (resolve an IR index step, then build+query) -

TEST(OramaCoreFulltextIndex, ResolvesViaRegistryAndAnswersAQuery) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    IndexRegistry registry;
    registry.add(std::make_shared<OramaCoreFulltextIndex>());
    EXPECT_TRUE(registry.has("oramacore_fulltext"));

    // Resolve straight from an IR `index` step naming this backend + the search kind.
    const zetta::ir::Node step{
        "step-index", zetta::ir::StepKind::Index, "build full-text index",
        {{"table", "articles"},
         {"kind", "search"},
         {"backend", "oramacore_fulltext"},
         {"fields", {"title", "body"}}}};
    auto backend = registry.resolve(step);
    ASSERT_TRUE(backend);
    EXPECT_EQ(backend->name(), "oramacore_fulltext");

    const IndexSpec spec = *zetta::index::index_spec_from_node(step);
    ASSERT_TRUE(backend->build(spec, sample_partitions()).ok());

    IndexQuery q;
    q.term = "postgres";
    EXPECT_EQ(backend->query(spec, q).row_ids,
              (std::vector<std::string>{"r1", "r3"}));
}
