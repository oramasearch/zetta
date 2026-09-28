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
//   create_experiment action create a seeded, replicated allocation definition
//   assign           action  assign first, then protect a proposed action
//   get_experiment   read    sizes, balance, erosion, and training rows
//   list_specs       read    the spec index (#646): OpenAPI + every capability
//                            and boundary schema as names, kinds, links
//   get_spec         read    one spec document by its list_specs name
//
// The #616 slice adds the typed detail reads + list_* siblings; the #617 slice
// adds governed action parity — the rest of the product loop as thin callers
// of the same engine entry points the GUI drives (capture_intent · import_n8n
// · compile_intent/n8n/python · gate · deploy · run · add_dataset ·
// testsuite_run; run/testsuite_run are `kind: job` and return the engine's durable
// Run id immediately). See docs/api/governed-actions.md +
// docs/api/adding-a-capability.md.
#include "zetta/agent/agent_record.hpp"

namespace zetta::api {

void register_core_capabilities(Registry& registry);

// The cloud-touching tools + the consent protocol (#621; called by
// register_core_capabilities; docs/api/consent.md):
//
//   cloud_inspect          action  consent-gated read-only provider inspect
//   detect_provisioners    action  consent-gated local-CLI context re-detect
//   add_nodes              action  permanent add; cloud targets consent-gated,
//                                  `local` executes directly (no cloud)
//   list_consent_requests  read    the node's consent ledger, results included
//   approve_consent        action  distinctly-privileged approval: executes
//                                  exactly the named action once
//   deny_consent           action  refuse (or withdraw) a pending challenge
void register_cloud_capabilities(Registry& registry);

// Pub/sub on the control surface (#1033, docs/pubsub/exposure.md): the
// registry entries exist on every node, but only topics created with
// `exposed` are reachable through them (engine-internal by default, D22).
//
//   create_topic    action  create a topic (exposed: false by default)
//   drop_topic      action  drop an exposed topic
//   list_topics     read    the exposed topics
//   publish_event   action  publish to an exposed topic
//   receive_events  read    long-poll an exposed topic from a sequence number
void register_pubsub_capabilities(Registry& registry);

// The versioned decision-policy control surface (#1104,
// docs/solutions/decision-policy.md).
void register_decision_policy_capabilities(Registry& registry);

// A Pipeline granted to an agent, run as that agent's tool (lake#238): the tool is
// `pipeline_<graph>` (lowercase, non-alphanumerics as `_`). The graph must still be approved at the
// version the grant pinned; it is gated, then run (`dataset`, `holdout`, `model_name` pass through).
// A failing gate or a changed approval is returned as the failure, and nothing runs.
ApiResult run_granted_pipeline(ApiContext& context,
                               const zetta::agent::AgentRecord& agent,
                               const std::string& tool,
                               const nlohmann::json& arguments);
std::string pipeline_tool_name(const std::string& graph);

}  // namespace zetta::api
