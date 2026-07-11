#pragma once

#include <string_view>

// The web interface (#647, docs/product/experience.md): a browser rendering of the cluster's read
// model + capability registry, served by the node's own Beast2 server at GET / — strictly a
// RENDERING of the same REST/MCP surfaces, never a fourth behavior path. Straight HTML + vanilla JS,
// embedded in the binary and served as ONE self-contained page (no framework, no build step, no
// external assets). Live data is fetched client-side from the existing REST endpoints
// (/v1/server_info for the read model, /v1/openapi.json for the whole capability registry, GET
// /v1/<capability> for each read), so the page can never drift from the surface it renders.
//
// The shell is PUBLIC (no secrets in it); the DATA endpoints keep their own bearer policy, so the
// page prompts for the token and sends it as `Authorization: Bearer` on every /v1/* fetch.
namespace zetta::api {

// GET / — the self-contained dashboard shell (static; the live data is fetched by the page's own JS
// under the node's bearer policy). Served as text/html; charset=utf-8.
std::string_view web_ui_page();

}  // namespace zetta::api
