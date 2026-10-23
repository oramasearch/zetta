// The llms.txt renderers (#645, docs/api/llms-endpoints.md). Every page is
// generated at request time from the capability registry's own descriptions
// and build version — the no-drift rule: this file adds structure (headings,
// ordering), never a second prose description of a capability.
#include "zetta/api/llms.hpp"

#include <string>

#include "zetta/api/registry.hpp"
#include "zetta/api/rest.hpp"
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

std::string render_capabilities(const Registry& registry) {
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

std::string render_cluster(const Registry& registry) {
    std::string out = "# Cluster\n\n";
    out += "Read cluster identity and membership with an authenticated "
           "capability call:\n\n";
    append_capability_line(out, registry, "server_info");
    append_capability_line(out, registry, "sql");
    out += "\nThe `system.nodes` view (via `sql`) lists every member with its "
           "observed state.\n";
    return out;
}

std::string render_data(const Registry& registry) {
    std::string out = "# Data\n\n";
    out += "Query databases, tables and the dataset-partition placement "
           "catalog through authenticated SQL. Results require the caller's "
           "resource permissions:\n\n";
    append_capability_line(out, registry, "sql");
    return out;
}

std::string render_indexes(const Registry& registry) {
    std::string out = "# Indexes\n\n";
    out += "Search an index with an authenticated capability call and "
           "permission to search that index:\n\n";
    append_capability_line(out, registry, "search");
    out += "\nThe `system.indexes` view (via `sql`) shows the same registry.\n";
    return out;
}

std::string render_projects(const Registry& registry) {
    std::string out = "# Projects\n\n";
    out += "Read projects through authenticated capabilities. Related "
           "runs, models and jobs are available through `system.runs`, "
           "`system.models` and `system.jobs` via authenticated SQL.\n\n";
    out += "Work with projects:\n\n";
    append_capability_line(out, registry, "list_projects");
    append_capability_line(out, registry, "create_project");
    return out;
}

std::string render_observability(const Registry& registry) {
    std::string out = "# Observability\n\n";
    out += "Every operation on this node records OpenTelemetry-style spans — "
           "intent, plan, timings, result shape — threaded by trace id. Every "
           "capability result carries `trace_id`, `span_id`, and "
           "`duration_ms`, so you can always read back what actually "
           "happened.\n\n";
    out += "Read them with authenticated capability calls:\n\n";
    append_capability_line(out, registry, "list_traces");
    append_capability_line(out, registry, "get_trace");
    append_capability_line(out, registry, "submit_feedback");
    out += "\nThe same spans are `system.traces` / `system.spans` via `sql`.\n";
    return out;
}

using TopicRenderer = std::string (*)(const Registry&);

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
        {{"cluster", "how to read cluster identity and membership"},
         render_cluster},
        {{"data",
          "how to query databases, tables, and the dataset-partition "
          "placement catalog"},
         render_data},
        {{"indexes", "how to query and search indexes"},
         render_indexes},
        {{"projects", "how to read projects, runs, models, and jobs"},
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

std::string render_llms_index(const Registry& registry) {
    std::string out = "# Orama Zetta\n\n";
    out += "> A zetta node: a distributed database + AI-project engine, "
           "driven entirely through a capability registry you can call over "
           "plain HTTP. This documentation describes the build's API; "
           "workspace inventory is available through authenticated "
           "capabilities.\n\n";
    out += "Version: " + std::string(zetta::kVersion) + "\n\n";

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
           "<token>` to authenticate). For example:\n\n";
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

std::optional<std::string> render_llms_topic(const Registry& registry,
                                             const std::string& topic) {
    for (const auto& entry : topic_entries()) {
        if (entry.topic.name == topic) return entry.render(registry);
    }
    return std::nullopt;
}

std::string render_llms_full(const Registry& registry) {
    std::string out = render_llms_index(registry);
    for (const auto& entry : topic_entries()) {
        out += "\n---\n\n" + entry.render(registry);
    }
    return out;
}

}  // namespace zetta::api
