#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"

// The **AI Solution Library** — a catalog of ready-to-deploy graph blueprints.
//
// Orama Zetta's product framing is *use-case-driven*: a **use case** (churn,
// recommendations, semantic search, …) is a generic AI-project type any team
// instantiates on its own data, and "the use cases are the consumable meals."
// This catalog is that menu — a curated set of named ExecutionGraph blueprints a
// user can pick from the Deploy panel, validate through the correctness gate, and
// place onto a target. It is the GUI's source of deployable graphs until a
// dedicated graph editor lands.
//
// The catalog is **data-driven and schema-validated at the boundary** (matching
// the Run record's approach): a Solution is parsed from JSON that is validated
// against the catalog JSON Schema, and each blueprint's `graph` rides in the IR's
// canonical JSON form and passes the same `zetta::ir::from_json` trust boundary
// the persistence/replication paths use. A malformed catalog is rejected with a
// human-readable reason instead of corrupting state.
//
// This slice ships a **built-in** catalog (SolutionCatalog::builtin()); loading
// user-authored catalogs from disk and instantiating a solution into a Project
// are later slices.
namespace zetta::pipeline {

// One AI solution blueprint: identity + presentation + the deployable graph.
struct Solution {
    std::string id;       // stable slug (e.g. "churn-prediction"); unique in a catalog
    std::string name;     // human name ("Customer Churn Prediction")
    std::string summary;  // one-line description of the outcome it delivers
    std::string category; // grouping label ("Customer Intelligence", "Search", …)
    ir::ExecutionGraph graph;  // the blueprint placed by deploy_graph
};

// Validate + parse a catalog document at the boundary. The document shape is
//   { "solutions": [ { id, name, summary, category, graph: { nodes, edges } } ] }
// It is checked against the catalog JSON Schema (see solution_catalog_schema()),
// and each solution's `graph` is then parsed via zetta::ir::from_json (which
// enforces the IR's structural rules). Non-throwing, matching zetta's
// no-exceptions convention: returns std::nullopt and, when `error` is non-null,
// sets it to a human-readable reason (e.g. `solutions[1].graph: edges[0]: edge
// references an unknown node`).
std::optional<std::vector<Solution>> parse_solution_catalog(
    const nlohmann::json& document, std::string* error = nullptr);

// The catalog JSON Schema (draft-07) — the single declarative source of truth for
// the document shape parse_solution_catalog() validates. Mirrors
// schemas/solution_catalog.schema.json; keep the two in sync.
const nlohmann::json& solution_catalog_schema();

// An ordered, read-only library of solution blueprints.
class SolutionCatalog {
public:
    explicit SolutionCatalog(std::vector<Solution> solutions)
        : solutions_(std::move(solutions)) {}

    // The built-in library: a process-wide, immutable catalog parsed once from
    // the embedded, schema-validated catalog document. Always valid (the embedded
    // data is covered by a test); empty only if that data ever fails to parse.
    static const SolutionCatalog& builtin();

    const std::vector<Solution>& solutions() const { return solutions_; }

    // The solution with this id, or nullptr if there is none. A **non-owning**
    // view into the catalog's stable storage, valid for the catalog's lifetime
    // (builtin() is a static singleton) — the catalog owns its solutions by
    // value, so this hands back an observer, not ownership.
    const Solution* find(const std::string& id) const;

    std::size_t size() const { return solutions_.size(); }
    bool empty() const { return solutions_.empty(); }

private:
    std::vector<Solution> solutions_;
};

}  // namespace zetta::pipeline