#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/gate.hpp"
#include "zetta/solution_catalog.hpp"

using zetta::default_gate;
using zetta::parse_solution_catalog;
using zetta::Solution;
using zetta::SolutionCatalog;
using zetta::solution_catalog_schema;

namespace {

// A minimal, valid one-solution catalog document — the smallest thing the
// boundary parser accepts. Tests below mutate a copy to exercise each rejection.
nlohmann::json valid_document() {
    return nlohmann::json::parse(R"JSON({
      "solutions": [
        {
          "id": "demo",
          "name": "Demo Solution",
          "summary": "A tiny end-to-end pipeline.",
          "category": "Examples",
          "graph": {
            "nodes": [
              { "id": "load", "kind": "connect", "name": "Load" },
              { "id": "serve", "kind": "serve", "name": "Serve" }
            ],
            "edges": [ { "from": "load", "to": "serve" } ]
          }
        }
      ]
    })JSON");
}

}  // namespace

// ---- the built-in AI Solution Library ---------------------------------------

TEST(SolutionCatalog, BuiltinIsNonEmptyAndValid) {
    const SolutionCatalog& catalog = SolutionCatalog::builtin();
    EXPECT_FALSE(catalog.empty());
    EXPECT_EQ(catalog.size(), catalog.solutions().size());

    // Every built-in blueprint is presentation-complete and carries a non-empty
    // graph that passes the correctness gate — i.e. it is actually deployable.
    const zetta::CorrectnessGate gate = default_gate();
    for (const Solution& solution : catalog.solutions()) {
        EXPECT_FALSE(solution.id.empty());
        EXPECT_FALSE(solution.name.empty());
        EXPECT_GT(solution.graph.node_count(), 0u);
        EXPECT_TRUE(gate.run(solution.graph).passed())
            << "built-in solution \"" << solution.id << "\" fails the gate";
    }
}

TEST(SolutionCatalog, FindResolvesByIdAndMissesGracefully) {
    const SolutionCatalog& catalog = SolutionCatalog::builtin();
    ASSERT_FALSE(catalog.empty());

    const Solution& first = catalog.solutions().front();
    const Solution* found = catalog.find(first.id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, first.id);

    EXPECT_EQ(catalog.find("no-such-solution"), nullptr);
}

TEST(SolutionCatalog, BuiltinIdsAreUnique) {
    const SolutionCatalog& catalog = SolutionCatalog::builtin();
    for (std::size_t outer = 0; outer < catalog.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < catalog.size(); ++inner) {
            EXPECT_NE(catalog.solutions()[outer].id,
                      catalog.solutions()[inner].id);
        }
    }
}

// ---- the boundary parser: accepts a valid document --------------------------

TEST(SolutionCatalog, ParsesAValidDocument) {
    std::string error;
    const std::optional<std::vector<Solution>> solutions =
        parse_solution_catalog(valid_document(), &error);
    ASSERT_TRUE(solutions) << error;
    ASSERT_EQ(solutions->size(), 1u);
    EXPECT_EQ((*solutions)[0].id, "demo");
    EXPECT_EQ((*solutions)[0].name, "Demo Solution");
    EXPECT_EQ((*solutions)[0].graph.node_count(), 2u);
    EXPECT_EQ((*solutions)[0].graph.edge_count(), 1u);
}

// ---- the boundary parser: rejects malformed documents with a reason ---------

TEST(SolutionCatalog, RejectsMissingRequiredField) {
    nlohmann::json document = valid_document();
    document["solutions"][0].erase("name");  // schema-required

    std::string error;
    EXPECT_FALSE(parse_solution_catalog(document, &error));
    EXPECT_FALSE(error.empty());
}

TEST(SolutionCatalog, RejectsUnknownStepKind) {
    nlohmann::json document = valid_document();
    document["solutions"][0]["graph"]["nodes"][0]["kind"] = "teleport";

    std::string error;
    EXPECT_FALSE(parse_solution_catalog(document, &error));
    EXPECT_FALSE(error.empty());
}

TEST(SolutionCatalog, RejectsUnknownTopLevelKey) {
    nlohmann::json document = valid_document();
    document["surprise"] = true;  // additionalProperties: false

    std::string error;
    EXPECT_FALSE(parse_solution_catalog(document, &error));
    EXPECT_FALSE(error.empty());
}

TEST(SolutionCatalog, RejectsDanglingEdgeViaIrBoundary) {
    // Shape is schema-valid, but the edge references a node that does not exist —
    // caught by zetta::ir::from_json, and the reason is path-qualified.
    nlohmann::json document = valid_document();
    document["solutions"][0]["graph"]["edges"][0]["to"] = "missing";

    std::string error;
    EXPECT_FALSE(parse_solution_catalog(document, &error));
    EXPECT_NE(error.find("solutions[0].graph"), std::string::npos) << error;
}

TEST(SolutionCatalog, RejectsDuplicateSolutionIds) {
    nlohmann::json document = valid_document();
    document["solutions"].push_back(document["solutions"][0]);  // same id twice

    std::string error;
    EXPECT_FALSE(parse_solution_catalog(document, &error));
    EXPECT_NE(error.find("duplicate"), std::string::npos) << error;
}

// ---- the schema accessor is a usable draft-07 document ----------------------

TEST(SolutionCatalog, SchemaAccessorIsAnObject) {
    const nlohmann::json& schema = solution_catalog_schema();
    EXPECT_TRUE(schema.is_object());
    EXPECT_TRUE(schema.contains("properties"));
}
