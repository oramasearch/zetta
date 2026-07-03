#pragma once

#include "zetta/api/registry.hpp"

// The core capability set — the first entries on the control surface
// (docs/api/registry-design.md → "What the skeleton ships"; the live table is
// docs/api/README.md → "Capabilities (v1)"):
//
//   server_info      read    node identity, version, entity counts, membership
//   sql              read    one governed SQL statement via QueryService —
//                            this alone exposes every system.* view
//   search           read    ranked fulltext via THE entry point Node::search
//                            (#641/#642): distributed scatter-gather when the
//                            table is partitioned; per-node provenance; one trace
//   list_indexes     read    the index registry (Node::indexes()) — discover
//                            what `search` can target (#642)
//   list_projects    read    project names
//   list_traces      read    recent observability spans (#640)
//   get_trace        read    every span of one trace id (#640)
//   submit_feedback  action  snapshot a feedback bundle as a large object (#640)
//   create_project   action  the action template: mutates, audited, replicated
//
// Each is a thin caller of an existing engine entry point; the wider parity
// sets land with #616/#617 following docs/api/adding-a-capability.md.
namespace zetta::api {

void register_core_capabilities(Registry& registry);

}  // namespace zetta::api
