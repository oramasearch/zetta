// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <vector>

// The llms.txt progressive-disclosure surface (#645, docs/api/llms-endpoints.md;
// the AX contract is docs/product/experience/ax.md): the node serves its own
// agent-readable documentation over plain HTTP — GET /llms.txt (what this
// product is, the first three reads, where everything lives), /llms/<topic>.txt
// per topic, /llms-full.txt — so a no-MCP agent orients with one GET.
//
// Generated from the capability registry and build version only (#1402): no
// Node access, workspace inventory or counts. Live reads use authenticated
// capabilities and their authorization checks. The HTTP-free renderers live in
// zetta-core; the Beast2 adapter serves every page as public discovery docs.
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

// GET /llms.txt — orientation: product and build version, the first
// three reads (their descriptions pulled from the registry), how to call a
// capability over POST /mcp, and the topic index.
std::string render_llms_index(const Registry& registry);

// GET /llms/<topic>.txt — one topic page; nullopt on an unknown topic (the
// caller answers 404 listing llms_topics()).
std::optional<std::string> render_llms_topic(const Registry& registry,
                                             const std::string& topic);

// GET /llms-full.txt — the index plus every topic page, one document.
std::string render_llms_full(const Registry& registry);

}  // namespace zetta::api
