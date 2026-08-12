#pragma once

#include <string>
#include <string_view>

// GET /mcp.md (#647-adjacent): human- and agent-readable Markdown on how to
// connect an MCP client to THIS node — served by the Beast2 server beside the
// /mcp endpoint it documents. PUBLIC by design: the connect instructions must be
// readable WITHOUT already holding the bearer token.
//
// Security contract (do not regress):
//   * The token VALUE is NEVER rendered — only whether one is required, plus a
//     placeholder in the example commands and how to fetch it from the secret.
//   * When the node is started WITH a token (`--mcp-token`), only the
//     authenticated instructions are shown — no unauthenticated option is
//     offered. When started WITHOUT one, only the unauthenticated option.
namespace zetta::api {

// Render the /mcp.md body. `origin` is the externally-visible scheme+host the
// request arrived on (e.g. "https://zetta.oramadev.net"); the endpoint is
// `origin + "/mcp"`. `token_required` reflects whether the node has a bearer
// token configured.
std::string render_mcp_md(std::string_view origin, bool token_required);

// Render the /cli.md body — the node's own command-line reference, so the
// Documentation nav can point at a real page instead of an entry that 404s.
//
// The flags here mirror `print_usage` in src/main.cpp. Two lists can drift, so
// `CliDocsDrift.EveryFlagIsDocumented` reads main.cpp and asserts every `--flag`
// it offers appears below. If you add a flag there, this fails until it is here.
std::string render_cli_md(std::string_view origin);

}  // namespace zetta::api
