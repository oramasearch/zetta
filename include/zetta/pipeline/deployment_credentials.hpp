#pragma once

#include <optional>
#include <string>

#include "zetta/connect/connection_store.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/step_authority.hpp"

namespace zetta { class Node; }

// Resolving a deployment target's `credentials_ref` (#1315, CTL-3). The
// reference names a saved connection (connect/connection_store.hpp), never the
// secret itself: `DeploymentTarget::from_json` refuses a reference that is not
// a connection name, so a secret pasted into it is refused at the boundary and
// never replicated or rendered in `system.deployments`. The deploy path
// resolves the name here, in memory, for the deploying principal, and fails
// closed. Docs: docs/pipeline/deployments.md, docs/governance/secret-references.md.
namespace zetta::pipeline {

// What resolving a target's credentials_ref gave.
struct DeploymentCredentials {
    // The saved connection, its opened credential included. Empty when the
    // target names none (nothing to present) or on a refusal.
    std::optional<connect::SavedConnection> connection;
    // Why it was refused; empty when it resolved (or there was nothing to
    // resolve). Never contains the credential.
    std::string error;
    // True when the refusal is the principal's lack of a grant (#1339), so a
    // caller answers `unauthorized` rather than `invalid_input`.
    bool unauthorized = false;

    bool ok() const { return error.empty(); }
};

// Resolve `target.credentials_ref` for `principal`. Fails closed: a name that
// is not a valid connection name, one that names no saved connection, a
// connection whose credential does not open on this node or that holds none,
// and a principal that may not use the connection
// (`connect::may_access_connection(..., Use)`: its owner, a `connections:use`
// holder flat or ON CONNECTION <name>, or the trusted local session) are all
// refused. A refusal for want of the grant is audited as `data_access` on
// `connection:<name>`, like every other refused use (#1339). An empty
// reference resolves to nothing. Owner thread only.
DeploymentCredentials resolve_deployment_credentials(Node& node,
                                                     const DeploymentTarget& target,
                                                     const RunPrincipal& principal);

}  // namespace zetta::pipeline
