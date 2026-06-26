#include "zetta/n8n_import.hpp"

#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/project.hpp"
#include "zetta/project_input.hpp"

namespace zetta {
namespace fs = std::filesystem;

namespace {

// The n8n workflow JSON Schema (draft-07). It pins the structural signature
// every n8n workflow shares — a `nodes` array of node-shaped objects + a
// `connections` object — without over-fitting to any one export flavor: UI
// "Download" adds `id`/`versionId`/`meta`/`pinData`/`tags`, the import form is
// minimal, and node `parameters` are node-type-specific. `additionalProperties`
// is left open everywhere so those extras pass, while non-workflow JSON (a bare
// array, a scalar, `{"foo":1}`) fails at the boundary. Storage-only: this checks
// the shape, it does not interpret the workflow.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "n8n.Workflow",
      "type": "object",
      "properties": {
        "name":   { "type": "string" },
        "active": { "type": "boolean" },
        "nodes": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "name":        { "type": "string" },
              "type":        { "type": "string" },
              "typeVersion": { "type": "number" },
              "position":    { "type": "array" },
              "parameters":  { "type": "object" }
            },
            "required": ["name", "type"]
          }
        },
        "connections": { "type": "object" }
      },
      "required": ["nodes", "connections"]
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
// validate() is then cheap and runs at every import.
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. Using the error-handler overload of
// validate() (rather than the throwing one) keeps validation failures from
// crossing the validator-library boundary as exceptions — the library's
// recommended path, and robust regardless of how each side is compiled (matches
// src/core/run.cpp).
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

// Schema-check an already-parsed document. Factored out so both the public
// string entry point (which parses first) reuses one validation path.
bool validate_against_schema(const nlohmann::json& document, std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(document, handler);
    if (handler.failed) {
        if (error) *error = "not an n8n workflow: " + handler.reason;
        return false;
    }
    return true;
}

}  // namespace

const nlohmann::json& n8n_workflow_schema() { return schema_document(); }

bool validate_n8n_workflow(const std::string& content, std::string* error) {
    const auto document =
        nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        if (error) *error = "invalid JSON";
        return false;
    }
    return validate_against_schema(document, error);
}

N8nImportResult import_n8n_workflow(Project& project, std::string content,
                                    std::string filename,
                                    std::string created_by) {
    N8nImportResult result;
    if (!validate_n8n_workflow(content, &result.error)) return result;

    // Store the raw bytes verbatim (the validated `content` is unmodified), so
    // the payload round-trips unchanged for later re-compilation. create_input
    // computes the SHA-256 itself (never trusted from the caller) and records
    // size + provenance.
    result.input = project.create_input(
        ProjectInputKind::Artifact, ProjectInputSource::Upload,
        std::move(content), std::move(filename), std::move(created_by));
    if (!result.input) result.error = "failed to store input";
    return result;
}

N8nImportResult import_n8n_workflow_file(Project& project, const fs::path& path,
                                         std::string created_by) {
    N8nImportResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "cannot open file: " + path.string();
        return result;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return import_n8n_workflow(project, std::move(content),
                               path.filename().string(), std::move(created_by));
}

}  // namespace zetta
