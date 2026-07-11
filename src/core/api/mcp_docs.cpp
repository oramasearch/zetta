#include "zetta/api/mcp_docs.hpp"

#include <string>
#include <string_view>

namespace zetta::api {

std::string render_mcp_md(std::string_view origin, bool token_required) {
    const std::string endpoint = std::string(origin) + "/mcp";
    std::string md;
    md.reserve(2048);

    md += "# Connect to this Zetta node over MCP\n\n";
    md += "This node speaks the [Model Context Protocol](https://modelcontextprotocol.io) "
          "over **streamable HTTP** (JSON-RPC request/response — no SSE). The endpoint is:\n\n";
    md += "```\nPOST " + endpoint + "\n```\n\n";

    if (token_required) {
        md += "## Authentication\n\n";
        md += "A **bearer token is required**. It is provisioned as a Kubernetes secret by the "
              "operator and is **not shown here** — read it from the secret (ask your operator "
              "for the exact name/namespace):\n\n";
        md += "```sh\nkubectl get secret <cluster>-zetta-mcp -n <namespace> "
              "-o jsonpath='{.data.token}' | base64 -d\n```\n\n";
        md += "Keep it out of your shell history:\n\n";
        md += "```sh\nread -rs ZETTA_MCP_TOKEN   # paste the token, then Enter\nexport ZETTA_MCP_TOKEN\n```\n\n";
        md += "### Claude Code\n\n```sh\nclaude mcp add --transport http zetta " + endpoint +
              " \\\n  --header \"Authorization: Bearer $ZETTA_MCP_TOKEN\"\n```\n\n";
        md += "### Claude Desktop / claude.ai\n\n"
              "Settings -> Connectors -> **Add custom connector** -> URL `" + endpoint +
              "`, then add the request header `Authorization: Bearer <token>`.\n\n";
        md += "### Smoke test (raw JSON-RPC)\n\n```sh\ncurl -s " + endpoint +
              " \\\n  -H \"Authorization: Bearer $ZETTA_MCP_TOKEN\" \\\n  -H 'Content-Type: application/json' \\\n"
              "  -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
              "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
              "\"clientInfo\":{\"name\":\"you\",\"version\":\"0\"}}}'\n```\n";
    } else {
        md += "## Authentication\n\n";
        md += "**None** — this endpoint is unauthenticated (local or trusted-network use only). "
              "Do not expose it to the public internet without a token.\n\n";
        md += "### Claude Code\n\n```sh\nclaude mcp add --transport http zetta " + endpoint + "\n```\n\n";
        md += "### Claude Desktop / claude.ai\n\n"
              "Settings -> Connectors -> **Add custom connector** -> URL `" + endpoint + "`.\n\n";
        md += "### Smoke test (raw JSON-RPC)\n\n```sh\ncurl -s " + endpoint +
              " \\\n  -H 'Content-Type: application/json' \\\n"
              "  -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
              "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
              "\"clientInfo\":{\"name\":\"you\",\"version\":\"0\"}}}'\n```\n";
    }

    md += "\n---\n\nOnce connected, the node exposes its capabilities as MCP tools "
          "(`tools/list`). The same surface is available as REST (`" + std::string(origin) +
          "/v1/openapi.json`) and for orientation as `" + std::string(origin) + "/llms.txt`.\n";
    return md;
}

}  // namespace zetta::api
