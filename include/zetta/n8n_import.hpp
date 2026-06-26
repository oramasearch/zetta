#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace zetta {

class Project;
class ProjectInput;

// Result of importing an n8n workflow as a project artifact input. On success
// `input` is the stored ProjectInput and `error` is empty; on failure `input`
// is null and `error` carries a human-readable reason.
struct N8nImportResult {
    std::shared_ptr<ProjectInput> input;  // the stored artifact; null on failure
    std::string error;                    // empty on success

    bool ok() const { return error.empty() && input != nullptr; }
};

// The n8n workflow JSON Schema (draft-07). One declarative source of truth for
// the structural signature of an n8n workflow export: an object with a `nodes`
// array of node-shaped objects (each with a string `name` + `type`) and a
// `connections` object. UI exports carry extra fields (`id`, `versionId`,
// `meta`, `pinData`, …); they're allowed (additionalProperties), so real
// exports validate while non-workflow JSON does not.
const nlohmann::json& n8n_workflow_schema();

// Validate that `content` is a well-formed n8n workflow: it must parse as JSON
// AND match n8n_workflow_schema(). Returns true on success; on failure returns
// false and sets *error (when non-null) to a human-readable reason. This is the
// storage boundary check — nothing here interprets or compiles the workflow.
bool validate_n8n_workflow(const std::string& content, std::string* error);

// Store a validated n8n workflow as an artifact ProjectInput on `project`
// (kind=Artifact, source=Upload). The raw bytes are kept byte-for-byte so the
// stored payload round-trips unchanged for later re-compilation; the
// content_hash (SHA-256), size, filename, created_by + created_at provenance are
// recorded via Project::create_input. Rejects content that is not a valid n8n
// workflow (invalid JSON or schema mismatch) without storing anything, returning
// an N8nImportResult whose `error` explains why.
N8nImportResult import_n8n_workflow(Project& project, std::string content,
                                    std::string filename,
                                    std::string created_by = {});

// File overload: read the raw bytes of `path` (its filename is recorded as the
// artifact's filename), then import as above. Reports an error if the file can't
// be opened.
N8nImportResult import_n8n_workflow_file(Project& project,
                                         const std::filesystem::path& path,
                                         std::string created_by = {});

}  // namespace zetta
