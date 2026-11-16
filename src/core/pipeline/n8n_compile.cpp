// SPDX-License-Identifier: MIT

#include "zetta/pipeline/n8n_compile.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/n8n_import.hpp"
#include "zetta/pipeline/project_input.hpp"

namespace zetta::pipeline {
namespace {

using ir::EdgeKind;
using ir::ExecutionGraph;
using ir::StepKind;

// Record the reason a workflow was rejected (when the caller wants it) and signal
// failure with an empty optional — the single bail-out for the compiler, mirroring
// ir::from_json's reject().
std::optional<ExecutionGraph> reject(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return std::nullopt;
}

// Build the IR from a document already known to satisfy the n8n workflow schema
// (so `nodes` is an array of objects each with a string `name` + `type`, and
// `connections` — if present — is an object). What the schema can't express is
// checked here in code: node names must be non-empty + unique (they key the
// graph), the node type must be one R1 can compile, and every connection must
// reference an existing node. These graph-semantic rules live in code exactly as
// they do in ir::from_json.
std::optional<ExecutionGraph> build_from_validated(const nlohmann::json& document,
                                                   std::string* error) {
    ExecutionGraph graph;

    // --- nodes → typed steps (keyed by the n8n node name) ------------------
    std::size_t node_index = 0;
    for (const auto& element : document["nodes"]) {
        const std::string at = "nodes[" + std::to_string(node_index++) + "]";
        const std::string name = element["name"].get<std::string>();
        if (name.empty()) {
            return reject(error, at + ": node has an empty name");
        }
        const std::string type = element["type"].get<std::string>();
        const std::optional<StepKind> kind = n8n_step_kind(type);
        if (!kind) {
            return reject(error, at + ": unsupported n8n node type \"" + type +
                                     "\" (node \"" + name + "\")");
        }
        // Provenance for later slices + traceability: the originating n8n node
        // type (and its id when the export carries one), namespaced under "n8n"
        // so it never collides with gate/annotation attribute keys.
        nlohmann::json provenance{{"type", type}};
        if (element.contains("id") && element["id"].is_string()) {
            provenance["id"] = element["id"];
        }
        nlohmann::json attributes{{"n8n", std::move(provenance)}};
        // The node name is the IR NodeId: n8n connections reference nodes by name
        // and names are unique within a workflow, so this makes edge resolution
        // exact. add_node returns an empty handle iff the id is already taken.
        if (!graph.add_node(*kind, name, std::move(attributes), /*id=*/name)) {
            return reject(error, at + ": duplicate node name \"" + name + "\"");
        }
    }

    // --- connections → DataFlow edges --------------------------------------
    // n8n's connection map is, per source node:
    //   connections[<source name>][<port type, e.g. "main">]
    //       = [ [ {node, type, index}, … ],  … ]   // outer array = output ports
    // The schema only pins `connections` as an object, so walk it defensively and
    // reject a malformed entry with a path-qualified reason.
    if (document.contains("connections")) {
        for (const auto& [source_name, by_port] : document["connections"].items()) {
            if (!graph.has_node(source_name)) {
                return reject(error, "connections: unknown source node \"" +
                                         source_name + "\"");
            }
            if (!by_port.is_object()) {
                return reject(error, "connections[\"" + source_name +
                                         "\"] must be an object");
            }
            for (const auto& [port_type, ports] : by_port.items()) {
                const std::string port_base =
                    "connections[\"" + source_name + "\"][\"" + port_type + "\"]";
                if (!ports.is_array()) {
                    return reject(error, port_base + " must be an array");
                }
                std::size_t port_index = 0;
                for (const auto& port : ports) {
                    const std::string port_at =
                        port_base + "[" + std::to_string(port_index++) + "]";
                    if (port.is_null()) continue;  // an unwired output port
                    if (!port.is_array()) {
                        return reject(error, port_at + " must be an array");
                    }
                    std::size_t conn_index = 0;
                    for (const auto& connection : port) {
                        const std::string conn_at =
                            port_at + "[" + std::to_string(conn_index++) + "]";
                        if (!connection.is_object() ||
                            !connection.contains("node") ||
                            !connection["node"].is_string()) {
                            return reject(error,
                                          conn_at + " needs a string \"node\"");
                        }
                        const std::string target =
                            connection["node"].get<std::string>();
                        if (!graph.has_node(target)) {
                            return reject(error, conn_at +
                                                     ": connection to unknown "
                                                     "node \"" +
                                                     target + "\"");
                        }
                        // n8n "main" connections carry data → DataFlow. Both
                        // endpoints exist, so add_edge returns false only for an
                        // exact-duplicate connection, which we silently dedupe
                        // (the IR holds no duplicate edges).
                        graph.add_edge(source_name, target, EdgeKind::DataFlow);
                    }
                }
            }
        }
    }

    return graph;
}

}  // namespace

std::optional<StepKind> n8n_step_kind(std::string_view n8n_type) {
    // R1 node-type → StepKind table (the churn workflows). Keyed on the exact n8n
    // `type` string; the table is the documented extension point as the use-case
    // catalog grows. An unmapped type is a hard miss (std::nullopt) so the
    // compiler fails loudly rather than guessing a wrong stage.
    static const std::unordered_map<std::string_view, StepKind> table = {
        // connect — triggers + data sources (pull data in / kick the pipeline off)
        {"n8n-nodes-base.scheduleTrigger", StepKind::Connect},
        {"n8n-nodes-base.cron", StepKind::Connect},
        {"n8n-nodes-base.manualTrigger", StepKind::Connect},
        {"n8n-nodes-base.webhook", StepKind::Connect},
        {"n8n-nodes-base.postgres", StepKind::Connect},
        // clean — reshape / compute features / normalize
        {"n8n-nodes-base.code", StepKind::Clean},
        {"n8n-nodes-base.function", StepKind::Clean},
        {"n8n-nodes-base.functionItem", StepKind::Clean},
        {"n8n-nodes-base.set", StepKind::Clean},
        // model — apply a model (R1 churn scores its model over an HTTP endpoint)
        {"n8n-nodes-base.httpRequest", StepKind::Model},
        // act — notify / write back to the world
        {"n8n-nodes-base.slack", StepKind::Act},
        {"n8n-nodes-base.emailSend", StepKind::Act},
    };
    const auto found = table.find(n8n_type);
    if (found == table.end()) return std::nullopt;
    return found->second;
}

std::optional<ExecutionGraph> compile_n8n_workflow(const std::string& content,
                                                   std::string* error) {
    // The same boundary check the importer uses — the workflow JSON enters the
    // compiler through its schema (parse + structural signature) before anything
    // is mapped. Sets *error to "invalid JSON" / "not an n8n workflow: …".
    if (!validate_n8n_workflow(content, error)) return std::nullopt;
    // Valid by construction; reparse (cheap for a workflow) to build the graph.
    const auto document =
        nlohmann::json::parse(content, nullptr, /*allow_exceptions=*/false);
    return build_from_validated(document, error);
}

std::optional<ExecutionGraph> compile_n8n_workflow(const ProjectInput& input,
                                                   std::string* error) {
    const std::optional<std::string> payload = input.read_payload();
    if (!payload) {
        return reject(error, "input payload not available locally");
    }
    return compile_n8n_workflow(*payload, error);
}

}  // namespace zetta::pipeline