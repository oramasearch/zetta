#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/methodology.hpp"

namespace zetta::cluster {
class ReplicationManager;
}

// Workspace methodologies (zetta#1261, lake#655): a workspace's own modeling
// approach, written as Python code in one of its repositories. The engine never
// runs that code (ADR-17): it registers the entry next to the catalogue, lists
// it, compiles pipelines that use it, refuses to verify one that has not been
// validated, and records what a run of it produced. Lake runs the code in its
// sandboxed runner and reports back through `record_methodology_run`.
//
// An entry is a replicated `methodology:<id>` value, id `W-<8 hex>`. The
// catalogue (`methodologies()`, ids `M-n`) is Orama's and read-only.
//
// See docs/ml/workspace-methodologies.md.
namespace zetta::ml {

// "W-" followed by eight lowercase hex digits.
bool is_workspace_methodology_id(std::string_view id);

// The model kind a workspace methodology's models and model steps carry:
// "python:<id>". Empty when `model_kind` names no workspace methodology.
std::string workspace_model_kind(std::string_view id);
std::string workspace_methodology_id_of_model_kind(std::string_view model_kind);

// The stored value's JSON Schema (draft-07): every field of an entry. The
// capabilities compose their input and output schemas from its properties.
const nlohmann::json& workspace_methodology_value_schema();

// Validate a stored value: the schema, then the rules a schema cannot say
// (a target kind of `none` exactly when no target is required, each
// parameter's default inside its bounds or choices, unique parameter and
// metric names). Returns the reason, or nullopt when the value is valid.
std::optional<std::string>
workspace_methodology_problem(const nlohmann::json& value);

// Runnable: validation passed for the entry's current code revision.
bool workspace_methodology_runnable(const nlohmann::json& value);

// The entry as `list_methodologies` and `get_methodology` answer it: the
// catalogue's fields (id, name, family, model_kinds, solutions, inputs,
// outputs, metrics, runnable, partial, issue, studies, study_reason,
// study_search_space), `source: workspace`, `editable: true`, then every stored
// field. Metrics are the reported names, `in_sample_<m>` and `held_out_<m>`.
nlohmann::json workspace_methodology_listing(const nlohmann::json& value);

// The shape `compile_intent` builds for a workspace methodology: connect (the
// pipeline's table, from hints `source`/`table` or `dataset_path`) →
// featurize → python_methodology (the model step: model_kind python:<id>,
// methodology <id>, parameters, target) → evaluate → register.
ir::ExecutionGraph build_workspace_methodology_shape(const nlohmann::json& value,
                                                     const ShapeRequest& request);

// The workspace methodology a graph's model step names (its `methodology`
// attribute or a `python:W-…` model_kind), or empty.
std::string workspace_methodology_of_graph(const ir::ExecutionGraph& graph);

// CRUD over the replicated `methodology:<id>` entries. The CRDT document is the
// source of truth: put/remove capture into it, get/list read its snapshot.
// Pure storage: id assignment and timestamps are the caller's. Non-throwing.
class WorkspaceMethodologyStore {
public:
    explicit WorkspaceMethodologyStore(zetta::cluster::ReplicationManager& replication);

    // Create or replace (captures `methodology:<id>`). nullopt (+ *error) when
    // the value is invalid.
    std::optional<nlohmann::json> put(const nlohmann::json& value, std::string* error);
    std::optional<nlohmann::json> get(const std::string& id) const;
    std::vector<nlohmann::json> list() const;  // id-ordered
    bool remove(const std::string& id);        // false if absent

    // A fresh `W-<8 hex>` id no entry holds.
    std::string new_id() const;

private:
    zetta::cluster::ReplicationManager& replication_;
};

} // namespace zetta::ml
