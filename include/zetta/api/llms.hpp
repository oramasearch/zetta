#pragma once

#include <optional>
#include <string>
#include <vector>

namespace zetta {
class Node;
}

// The llms.txt progressive-disclosure surface (#645, docs/api/llms-endpoints.md;
// the AX contract is docs/product/experience/ax.md): the node serves its own
// agent-readable documentation over plain HTTP — GET /llms.txt (what this
// cluster is, the first three reads, where everything lives), /llms/<topic>.txt
// per topic, /llms-full.txt — so a no-MCP agent orients with one GET.
//
// Everything is GENERATED at request time from the capability registry's own
// descriptions plus live Node reads (node id, membership, databases, indexes,
// the dataset-partition catalog) — never a second prose source, so the pages
// can never drift from the surface they document. The renderers are HTTP-free
// (zetta-core); the Beast2 adapter TU (src/core/api/http_server.cpp) serves
// them beside POST /mcp under the same bearer policy.
namespace zetta::api {

class Registry;

// One /llms/<name>.txt page. `summary` is the one-line hook shown on the
// /llms.txt topic index.
struct LlmsTopic {
    std::string name;
    std::string summary;
};

// The topic pages this node serves, in index order.
const std::vector<LlmsTopic>& llms_topics();

// GET /llms.txt — orientation: what this node is (live facts), the first
// three reads (their descriptions pulled from the registry), how to call a
// capability over POST /mcp, and the topic index.
std::string render_llms_index(const Node& node, const Registry& registry);

// GET /llms/<topic>.txt — one topic page; nullopt on an unknown topic (the
// caller answers 404 listing llms_topics()).
std::optional<std::string> render_llms_topic(const Node& node,
                                             const Registry& registry,
                                             const std::string& topic);

// GET /llms-full.txt — the index plus every topic page, one document.
std::string render_llms_full(const Node& node, const Registry& registry);

}  // namespace zetta::api
