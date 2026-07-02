#include "zetta/pipeline/solution_catalog.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/ir/serialization.hpp"

namespace zetta {
namespace {

// The catalog JSON Schema (draft-07). One declarative source of truth for the
// document shape: a list of solutions, each carrying presentation metadata and a
// blueprint `graph` in the IR's canonical JSON form. The schema fixes the shape,
// types, and the step/edge-kind enums; the IR's own structural rules (unique node
// ids, no dangling edges) are then enforced by zetta::ir::from_json — layered
// boundary validation, exactly like the Run record (schema + typed parse).
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.SolutionCatalog",
      "type": "object",
      "properties": {
        "solutions": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id":       { "type": "string", "minLength": 1 },
              "name":     { "type": "string", "minLength": 1 },
              "summary":  { "type": "string" },
              "category": { "type": "string" },
              "graph": {
                "type": "object",
                "properties": {
                  "nodes": {
                    "type": "array",
                    "items": {
                      "type": "object",
                      "properties": {
                        "id":   { "type": "string", "minLength": 1 },
                        "kind": { "type": "string",
                                  "enum": ["connect", "clean", "model",
                                           "index", "serve", "act"] },
                        "name": { "type": "string" },
                        "attributes": { "type": "object" }
                      },
                      "required": ["id", "kind", "name"],
                      "additionalProperties": false
                    }
                  },
                  "edges": {
                    "type": "array",
                    "items": {
                      "type": "object",
                      "properties": {
                        "from": { "type": "string", "minLength": 1 },
                        "to":   { "type": "string", "minLength": 1 },
                        "kind": { "type": "string",
                                  "enum": ["data_flow", "control"] }
                      },
                      "required": ["from", "to"],
                      "additionalProperties": false
                    }
                  }
                },
                "required": ["nodes", "edges"],
                "additionalProperties": false
              }
            },
            "required": ["id", "name", "summary", "category", "graph"],
            "additionalProperties": false
          }
        }
      },
      "required": ["solutions"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. The error-handler overload of validate()
// (rather than the throwing one) keeps validation failures from crossing the
// validator-library boundary as exceptions — robust regardless of compilation.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// The built-in AI Solution Library. Each blueprint is a small but representative
// pipeline over the connect→clean→model→index→serve→act taxonomy; together they
// cover the common AI use-case shapes. Authored in the catalog's JSON form so it
// travels the same schema + IR boundary validation as any external catalog would.
const char* kBuiltinCatalog = R"JSON({
  "solutions": [
    {
      "id": "churn-prediction",
      "name": "Customer Churn Prediction",
      "summary": "Predict which customers are likely to churn and trigger retention.",
      "category": "Customer Intelligence",
      "graph": {
        "nodes": [
          { "id": "load_customers", "kind": "connect", "name": "Load customers",
            "attributes": { "source": "postgres", "table": "customers" } },
          { "id": "load_events", "kind": "connect", "name": "Load usage events",
            "attributes": { "source": "warehouse", "table": "events" } },
          { "id": "build_features", "kind": "clean", "name": "Build features" },
          { "id": "train_churn", "kind": "model", "name": "Train churn model",
            "attributes": { "algo": "xgboost", "target": "churned" } },
          { "id": "rank_at_risk", "kind": "index", "name": "Rank at-risk accounts",
            "attributes": { "kind": "score" } },
          { "id": "churn_api", "kind": "serve", "name": "Serve churn scores" },
          { "id": "notify_retention", "kind": "act", "name": "Notify retention" }
        ],
        "edges": [
          { "from": "load_customers", "to": "build_features" },
          { "from": "load_events", "to": "build_features" },
          { "from": "build_features", "to": "train_churn" },
          { "from": "train_churn", "to": "rank_at_risk" },
          { "from": "rank_at_risk", "to": "churn_api" },
          { "from": "churn_api", "to": "notify_retention" }
        ]
      }
    },
    {
      "id": "product-recommendations",
      "name": "Product Recommendations",
      "summary": "Recommend the next-best product per user from behavioral signals.",
      "category": "Personalization",
      "graph": {
        "nodes": [
          { "id": "load_interactions", "kind": "connect", "name": "Load interactions",
            "attributes": { "source": "events", "table": "interactions" } },
          { "id": "sessionize", "kind": "clean", "name": "Sessionize" },
          { "id": "train_recommender", "kind": "model", "name": "Train recommender",
            "attributes": { "algo": "als" } },
          { "id": "build_item_index", "kind": "index", "name": "Build item index",
            "attributes": { "kind": "vector" } },
          { "id": "reco_api", "kind": "serve", "name": "Serve recommendations" }
        ],
        "edges": [
          { "from": "load_interactions", "to": "sessionize" },
          { "from": "sessionize", "to": "train_recommender" },
          { "from": "train_recommender", "to": "build_item_index" },
          { "from": "build_item_index", "to": "reco_api" }
        ]
      }
    },
    {
      "id": "semantic-search",
      "name": "Semantic Document Search",
      "summary": "Embed documents and serve natural-language semantic search.",
      "category": "Search",
      "graph": {
        "nodes": [
          { "id": "load_documents", "kind": "connect", "name": "Load documents",
            "attributes": { "source": "bucket", "format": "html" } },
          { "id": "chunk_text", "kind": "clean", "name": "Chunk text" },
          { "id": "embed_and_index", "kind": "index", "name": "Embed and index",
            "attributes": { "kind": "vector", "model": "text-embedding" } },
          { "id": "search_api", "kind": "serve", "name": "Serve search" }
        ],
        "edges": [
          { "from": "load_documents", "to": "chunk_text" },
          { "from": "chunk_text", "to": "embed_and_index" },
          { "from": "embed_and_index", "to": "search_api" }
        ]
      }
    },
    {
      "id": "lead-scoring",
      "name": "Sales Lead Scoring",
      "summary": "Score inbound leads by conversion likelihood for prioritization.",
      "category": "Revenue",
      "graph": {
        "nodes": [
          { "id": "load_crm", "kind": "connect", "name": "Load CRM leads",
            "attributes": { "source": "crm", "table": "leads" } },
          { "id": "enrich_leads", "kind": "clean", "name": "Enrich leads" },
          { "id": "score_leads", "kind": "model", "name": "Score leads",
            "attributes": { "algo": "logistic_regression" } },
          { "id": "scoring_api", "kind": "serve", "name": "Serve lead scores" }
        ],
        "edges": [
          { "from": "load_crm", "to": "enrich_leads" },
          { "from": "enrich_leads", "to": "score_leads" },
          { "from": "score_leads", "to": "scoring_api" }
        ]
      }
    },
    {
      "id": "support-triage",
      "name": "Support Ticket Triage",
      "summary": "Classify and route incoming support tickets automatically.",
      "category": "Operations",
      "graph": {
        "nodes": [
          { "id": "load_tickets", "kind": "connect", "name": "Load tickets",
            "attributes": { "source": "helpdesk", "stream": "tickets" } },
          { "id": "normalize_text", "kind": "clean", "name": "Normalize text" },
          { "id": "classify_intent", "kind": "model", "name": "Classify intent",
            "attributes": { "algo": "llm" } },
          { "id": "route_ticket", "kind": "act", "name": "Route ticket" }
        ],
        "edges": [
          { "from": "load_tickets", "to": "normalize_text" },
          { "from": "normalize_text", "to": "classify_intent" },
          { "from": "classify_intent", "to": "route_ticket" }
        ]
      }
    }
  ]
})JSON";

}  // namespace

const nlohmann::json& solution_catalog_schema() { return schema_document(); }

std::optional<std::vector<Solution>> parse_solution_catalog(
    const nlohmann::json& document, std::string* error) {
    // 1) Validate the document shape against the catalog schema at the boundary.
    collecting_error_handler handler;
    schema_validator().validate(document, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return std::nullopt;
    }

    // 2) Build each Solution, parsing its blueprint through the IR trust boundary
    //    (unique node ids, no dangling/duplicate edges) so a structurally invalid
    //    graph is rejected with the IR's own diagnostic, not silently accepted.
    std::vector<Solution> solutions;
    const nlohmann::json& items = document["solutions"];
    solutions.reserve(items.size());
    std::size_t index = 0;
    for (const nlohmann::json& item : items) {
        const std::string at = "solutions[" + std::to_string(index++) + "]";
        std::string graph_error;
        std::optional<ir::ExecutionGraph> graph =
            ir::from_json(item["graph"], &graph_error);
        if (!graph) {
            if (error) *error = at + ".graph: " + graph_error;
            return std::nullopt;
        }
        solutions.push_back(Solution{item["id"].get<std::string>(),
                                     item["name"].get<std::string>(),
                                     item["summary"].get<std::string>(),
                                     item["category"].get<std::string>(),
                                     std::move(*graph)});
    }

    // Ids must be unique within a catalog (find() resolves by id, and the Deploy
    // panel selects by id) — reject duplicates rather than shadow silently.
    for (std::size_t outer = 0; outer < solutions.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < solutions.size(); ++inner) {
            if (solutions[outer].id == solutions[inner].id) {
                if (error) {
                    *error = "duplicate solution id \"" + solutions[outer].id + "\"";
                }
                return std::nullopt;
            }
        }
    }
    return solutions;
}

const SolutionCatalog& SolutionCatalog::builtin() {
    static const SolutionCatalog catalog = [] {
        const nlohmann::json document =
            nlohmann::json::parse(kBuiltinCatalog, nullptr,
                                  /*allow_exceptions=*/false);
        std::string error;
        std::optional<std::vector<Solution>> solutions =
            document.is_discarded() ? std::nullopt
                                    : parse_solution_catalog(document, &error);
        if (!solutions) {
            // The embedded catalog is our own data and covered by a test, so this
            // is unreachable in practice; fail loud and degrade to an empty
            // library rather than crash the headed app.
            std::fprintf(stderr, "zetta: built-in solution catalog invalid (%s)\n",
                         document.is_discarded() ? "unparseable JSON"
                                                 : error.c_str());
            return SolutionCatalog{{}};
        }
        return SolutionCatalog{std::move(*solutions)};
    }();
    return catalog;
}

const Solution* SolutionCatalog::find(const std::string& id) const {
    for (const Solution& solution : solutions_) {
        if (solution.id == id) return &solution;
    }
    return nullptr;
}

}  // namespace zetta
