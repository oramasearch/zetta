#pragma once

#include "zetta/api/registry.hpp"

// The core capability set — the first entries on the control surface
// (docs/api/registry-design.md → "What the skeleton ships"):
//
//   server_info     read    node identity, version, entity counts, membership
//   sql             read    one governed SQL statement via QueryService —
//                           this alone exposes every system.* view
//   list_projects   read    project names
//   create_project  action  the action template: mutates, audited, replicated
//
// Each is a thin caller of an existing engine entry point; the wider parity
// sets land with #616/#617 following docs/api/adding-a-capability.md.
namespace zetta::api {

void register_core_capabilities(Registry& registry);

}  // namespace zetta::api
