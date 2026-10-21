#pragma once

#include <optional>
#include <string>
#include <utility>

#include "zetta/governance/access_key.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"

// Step authority (#843): what a graph step may do is decided when the step runs,
// from the run's principal, never from the node process.
//
// Before this, the only check was at submission: the caller needed the right to
// call `run` (or `start_run` in the console), and every step then executed with
// the node's full authority. A principal allowed to start a run could therefore
// make a connect step read a table it may not read, or fire an act step it may
// not fire. Now each step names the right it exercises (`step_operation`), and
// the run's principal must hold that right (deny-by-default) before the step
// executes. A connect step that names a table is also checked against the
// principal's object-scoped grants on that table.
//
// Every decision for a named principal, granted or denied, is one `data_access`
// event on the existing audit trail (`Node::record_audit_event`). The detail is in
// docs/governance/step-authority.md.
namespace zetta { class Node; }

namespace zetta::pipeline {

// The identity a run executes as: the acting identity of the call that started
// it. `acting_id` is a user id when `acting_is_user`, otherwise an AccessKey id.
// An empty `acting_id` is the trusted local session (the headed GUI, a console
// with no key, in-process callers): the same identity every other gate in the
// node lets through without a check, so steps run unchecked and unaudited here.
struct RunPrincipal {
    std::string acting_id;
    bool acting_is_user = false;

    bool local() const { return acting_id.empty(); }
};

// The right a step of `kind` exercises when it runs. The rights are the
// existing permission vocabulary's (governance/permission.cpp), so a role that
// may run a pipeline end to end needs no new atom:
//
//   connect -> data/read                     (sql:read)
//   clean   -> orchestration/create_dataset_from_query (pipelines:manage: it derives
//              a dataset from data the run already read; it reaches no new source)
//   model   -> orchestration/train_model     (pipelines:manage)
//   index   -> orchestration/create_index    (pipelines:manage)
//   serve   -> data/predict                  (serving:read: it answers from a model;
//              exposing an endpoint stays the separate expose_model call)
//   act     -> orchestration/deliver_action_task (pipelines:manage)
zetta::governance::Operation step_operation(ir::StepKind kind);

// The `database.table` a connect step names through its `source` + `table`
// attributes, when both are non-empty strings. The step's read is then also
// checked against the principal's grants on that table.
std::optional<std::pair<std::string, std::string>> step_table(const ir::Node& step);

// The decision alone, with no audit record: may `principal` run `step` on `node`?
// The local principal is always allowed. A user is checked against its roles
// (Node::authorize_user), an AccessKey against its capabilities
// (Node::authorize); an unknown or disabled identity is denied. A connect step
// naming a table is allowed for a user whose sql:read is scoped to that table.
zetta::governance::AccessDecision decide_step(const Node& node,
                                              const RunPrincipal& principal,
                                              const ir::Node& step);

// The decision, recorded: as decide_step, plus one `data_access` audit event for
// a named principal (actor = the principal, target = `step:<kind>:<label>`,
// allowed = the decision, detail = the right, the run when known, the reason on a
// denial). Must run on the node's owner thread (the audit append).
zetta::governance::AccessDecision authorize_step(Node& node,
                                                 const RunPrincipal& principal,
                                                 const ir::Node& step,
                                                 const std::string& run_id = {});

// Authorize every step of `graph` before anything runs (used where a run does not
// walk its steps through GraphExecutor, e.g. a methodology's own run callback).
// Each step is authorized and recorded in graph order; the first denial stops
// the walk and is returned. Must run on the node's owner thread.
struct GraphAuthorization {
    bool allowed = true;
    std::string step_id;  // the denied step's id (when !allowed)
    std::string reason;   // why (when !allowed)

    explicit operator bool() const { return allowed; }
};
GraphAuthorization authorize_graph_steps(Node& node, const RunPrincipal& principal,
                                         const ir::ExecutionGraph& graph,
                                         const std::string& run_id = {});

}  // namespace zetta::pipeline
