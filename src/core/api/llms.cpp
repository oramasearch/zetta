// The llms.txt renderers (#645, docs/api/llms-endpoints.md). Every page is
// generated at request time from the capability registry's own descriptions
// plus live Node reads — the no-drift rule: this file adds structure (headings,
// ordering), never a second prose description of a capability.
#include "zetta/api/llms.hpp"

#include <string>

#include "zetta/api/registry.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/data/database.hpp"
#include "zetta/index/index.hpp"
#include "zetta/node.hpp"
#include "zetta/version.hpp"

namespace zetta::api {

namespace {

// One capability rendered from its registry entry, verbatim: name, kind, the
// required operation, the flags, the description, and the input schema.
void append_capability(std::string& out, const Capability& capability) {
    out += "### `" + capability.name + "` (" +
           std::string(to_string(capability.kind)) + " · requires " +
           std::string(zetta::governance::to_string(
               capability.required_operation.domain)) +
           ":" + capability.required_operation.action;
    if (capability.mutates) out += " · mutates";
    if (capability.requires_consent)
        out += " \u00b7 consent-gated (only with --governance)";
    out += ")\n\n";
    out += capability.description + "\n\n";
    out += "REST: " + rest_method(capability) + " " + rest_path(capability) +
           " (#618)\n\n";
    out += "Input schema: " + capability.input_schema.dump() + "\n\n";
    if (capability.output_schema.is_object() &&
        !capability.output_schema.empty()) {
        out += "Output schema: " + capability.output_schema.dump() + "\n\n";
    }
}

// One line per capability, pulled from the registry (used by topic pages that
// point at their related capabilities).
void append_capability_line(std::string& out, const Registry& registry,
                            const std::string& name) {
    const auto capability = registry.find(name);
    if (!capability) return;
    out += "- `" + capability->name + "` — " + capability->description + "\n";
}

// ---- topic renderers --------------------------------------------------------

std::string render_capabilities(const Node&, const Registry& registry) {
    std::string out = "# Capabilities\n\n";
    out += "Every entry on this node's control surface, rendered live from "
           "the capability registry (`tools/list` over POST /mcp returns the "
           "same set). Every capability also accepts the reserved optional "
           "`intent` field — say what you are trying to do; it is recorded on "
           "the call's trace.\n\n";
    for (const auto& capability : registry.list()) {
        append_capability(out, *capability);
    }
    return out;
}

std::string render_cluster(const Node& node, const Registry& registry) {
    std::string out = "# Cluster\n\n";
    out += "This node, live:\n\n";
    out += "- node id: " + node.node_id() + "\n";
    out += "- version: " + std::string(zetta::kVersion) + "\n";
    const auto members = node.cluster_members();
    out += "- cluster members (" + std::to_string(members.size()) + "):\n";
    for (const auto& member : members) {
        out += "  - " + member +
               (member == node.node_id() ? "  (this node)" : "") + "\n";
    }
    out += "\nRead the same facts programmatically:\n\n";
    append_capability_line(out, registry, "server_info");
    append_capability_line(out, registry, "sql");
    out += "\nThe `system.nodes` view (via `sql`) lists every member with its "
           "observed state.\n";
    return out;
}

std::string render_data(const Node& node, const Registry& registry) {
    std::string out = "# Data\n\n";
    out += "Databases and tables on this node, live:\n\n";
    const auto database_names = node.database_names();
    if (database_names.empty()) {
        out += "(no databases yet)\n";
    }
    for (const auto& database_name : database_names) {
        const auto database = node.find_database(database_name);
        if (!database) continue;
        out += "- database `" + database_name + "`";
        const auto table_names = database->table_names();
        if (table_names.empty()) {
            out += " (no tables)\n";
            continue;
        }
        out += " — tables:";
        for (const auto& table_name : table_names) {
            out += " `" + table_name + "`";
        }
        out += "\n";
    }
    const auto partitions = node.dataset_partitions();
    out += "\nDataset partitions (the replicate-to-all placement catalog): " +
           std::to_string(partitions.size()) + "\n";
    for (const auto& partition : partitions) {
        out += "- `" + partition->id() + "` — table " + partition->table() +
               ", rows [" + std::to_string(partition->row_range().start) +
               ", " + std::to_string(partition->row_range().end) + "), " +
               std::to_string(partition->owner_nodes().size()) +
               " owner node(s)\n";
    }
    out += "\nQuery any of it (every `system.*` view included):\n\n";
    append_capability_line(out, registry, "sql");
    return out;
}

std::string render_indexes(const Node& node, const Registry& registry) {
    std::string out = "# Indexes\n\n";
    const auto indexes = node.indexes();
    out += "Registered indexes on this node, live (" +
           std::to_string(indexes.size()) + "):\n\n";
    if (indexes.empty()) {
        out += "(no indexes yet)\n";
    }
    for (const auto& index : indexes) {
        out += "- `" + index->id() + "` — " + std::string(index->kind_name()) +
               " index over table `" + index->table() + "` (fields:";
        for (const auto& field : index->fields()) {
            out += " `" + field + "`";
        }
        out += ") via backend `" + index->backend() + "`, created " +
               index->created() + "\n";
    }
    out += "\nUse them:\n\n";
    append_capability_line(out, registry, "search");
    out += "\nThe `system.indexes` view (via `sql`) shows the same registry.\n";
    return out;
}

std::string render_projects(const Node& node, const Registry& registry) {
    std::string out = "# Projects\n\n";
    const auto project_names = node.project_names();
    out += "AI projects on this node, live (" +
           std::to_string(project_names.size()) + "):\n\n";
    if (project_names.empty()) {
        out += "(no projects yet)\n";
    }
    for (const auto& project_name : project_names) {
        out += "- `" + project_name + "`\n";
    }
    out += "\nRelated engine state, live: " +
           std::to_string(node.runs().size()) + " run(s), " +
           std::to_string(node.models().size()) + " model(s), " +
           std::to_string(node.jobs().size()) + " job(s) — all readable as "
           "`system.runs` / `system.models` / `system.jobs` via `sql`.\n";
    out += "\nWork with projects:\n\n";
    append_capability_line(out, registry, "list_projects");
    append_capability_line(out, registry, "create_project");
    return out;
}

std::string render_observability(const Node& node, const Registry& registry) {
    std::string out = "# Observability\n\n";
    out += "Every operation on this node records OpenTelemetry-style spans — "
           "intent, plan, timings, result shape — threaded by trace id. Every "
           "capability result carries `trace_id`, `span_id`, and "
           "`duration_ms`, so you can always read back what actually "
           "happened.\n\n";
    out += "Recent spans on this node, live: " +
           std::to_string(node.span_recorder().recent().size()) + "\n\n";
    out += "Read them:\n\n";
    append_capability_line(out, registry, "list_traces");
    append_capability_line(out, registry, "get_trace");
    append_capability_line(out, registry, "submit_feedback");
    out += "\nThe same spans are `system.traces` / `system.spans` via `sql`.\n";
    return out;
}

using TopicRenderer = std::string (*)(const Node&, const Registry&);

struct TopicEntry {
    LlmsTopic topic;
    TopicRenderer render;
};

const std::vector<TopicEntry>& topic_entries() {
    static const std::vector<TopicEntry> entries = {
        {{"capabilities",
          "every capability on the control surface, with kind, required "
          "operation, description, and input schema"},
         render_capabilities},
        {{"cluster", "this node's id, version, and live cluster membership"},
         render_cluster},
        {{"data",
          "live databases, tables, and the dataset-partition placement "
          "catalog"},
         render_data},
        {{"indexes", "live registered indexes and how to search them"},
         render_indexes},
        {{"projects", "live AI projects, runs, models, and jobs"},
         render_projects},
        {{"observability",
          "traces and spans: how to read back what actually happened"},
         render_observability},
    };
    return entries;
}

}  // namespace

const std::vector<LlmsTopic>& llms_topics() {
    static const std::vector<LlmsTopic> topics = [] {
        std::vector<LlmsTopic> list;
        for (const auto& entry : topic_entries()) list.push_back(entry.topic);
        return list;
    }();
    return topics;
}

std::string render_llms_index(const Node& node, const Registry& registry) {
    std::string out = "# Orama Zetta node " + node.node_id() + "\n\n";
    out += "> A zetta node: a distributed database + AI-project engine, "
           "driven entirely through a capability registry you can call over "
           "plain HTTP. This page is generated live from that registry and "
           "this node's state — it is always current.\n\n";
    out += "Live facts: version " + std::string(zetta::kVersion) + " · " +
           std::to_string(node.cluster_members().size()) +
           " cluster member(s) · " + std::to_string(node.database_count()) +
           " database(s) · " + std::to_string(node.project_count()) +
           " project(s) · " + std::to_string(node.index_count()) +
           " index(es) · " + std::to_string(node.dataset_partition_count()) +
           " dataset partition(s)\n\n";

    out += "## First three reads\n\n";
    out += "Orient by calling these, in order:\n\n";
    int position = 1;
    for (const std::string name : {"server_info", "sql", "list_traces"}) {
        const auto capability = registry.find(name);
        if (!capability) continue;
        out += std::to_string(position++) + ". `" + capability->name + "` — " +
               capability->description + "\n";
    }

    out += "\n## How to call a capability\n\n";
    out += "Every capability is an MCP tool served by this same host and "
           "port: JSON-RPC 2.0 over `POST /mcp` (add `Authorization: Bearer "
           "<token>` if this node requires one). For example:\n\n";
    out += "    POST /mcp\n";
    out += "    {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
           "\"params\":{\"name\":\"server_info\",\"arguments\":{}}}\n\n";
    out += "`tools/list` returns every capability with its full input and "
           "output schemas. Every result carries a `trace_id` — read back "
           "what happened with `get_trace`.\n";
    out += "\nThe same registry is also plain REST (#618): reads are `GET "
           "/v1/<name>` (input in the query string), actions are `POST "
           "/v1/<name>` (input as the JSON body), and `GET /v1/openapi.json` "
           "is the OpenAPI document generated from the same schemas. For "
           "example:\n\n";
    out += "    GET /v1/server_info\n";
    out += "    GET /v1/sql?statement=SELECT%20*%20FROM%20system.datasets;\n";

    out += "\n## Topics\n\n";
    for (const auto& topic : llms_topics()) {
        out += "- [" + topic.name + "](/llms/" + topic.name +
               ".txt): " + topic.summary + "\n";
    }
    out += "- [llms-full.txt](/llms-full.txt): every topic on one page\n";
    return out;
}

std::optional<std::string> render_llms_topic(const Node& node,
                                             const Registry& registry,
                                             const std::string& topic) {
    for (const auto& entry : topic_entries()) {
        if (entry.topic.name == topic) return entry.render(node, registry);
    }
    return std::nullopt;
}

std::string render_llms_full(const Node& node, const Registry& registry) {
    std::string out = render_llms_index(node, registry);
    for (const auto& entry : topic_entries()) {
        out += "\n---\n\n" + entry.render(node, registry);
    }
    return out;
}

}  // namespace zetta::api
