#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// The web interface (#647, docs/product/experience.md): a browser rendering of the cluster's read
// model AND its capability registry, served by the node's own Beast2 server. Unlike the earlier
// client-JS shell, this is **server-rendered straight HTML** — the node queries its own read model +
// registry and emits full HTML pages. Auth rides an HttpOnly `zetta_session` cookie (its value is a
// bearer token, validated identically to the Authorization header) so a plain browser navigation
// stays authenticated; the login form exchanges a token for that cookie. It is still STRICTLY A
// RENDERING/INVOCATION of the existing capability surface — never a new behavior path. One
// self-contained document per page (inline CSS, no framework, no external assets), responsive
// (mobile-first), following the Orama design system (light surfaces, ink text, surgical coral,
// semantic status). These are PURE renderers: data in, HTML out — the http_server adapter does the
// auth + querying and hands the JSON in.
namespace zetta::api {

// HTML-escape text for safe interpolation into an element body or a double-quoted attribute.
std::string web_ui_escape(std::string_view text);

// The login page — a full HTML document. `error` shows a message when non-empty.
std::string web_ui_login(std::string_view error);

// Wrap `content` in the full page shell (sticky top bar + responsive nav). `active` is the current
// nav key, `user` the acting user's label, `meta` the node meta line (id · version · N nodes).
std::string web_ui_shell(std::string_view active, std::string_view user, std::string_view meta,
                         std::string_view content);

// A titled panel wrapper (card). `sub` is an optional subtitle.
std::string web_ui_panel(std::string_view title, std::string_view sub, std::string_view body);

// Render a SQL/read result (an ordered `columns` array + a `rows` array of column-keyed objects) as
// an on-brand, horizontally-scrollable HTML table. Column-name heuristics format status/bytes/bools.
std::string web_ui_table(const nlohmann::json& columns, const nlohmann::json& rows);

// Render the overview count cards from a `server_info` value object.
std::string web_ui_overview_cards(const nlohmann::json& server_info);

}  // namespace zetta::api
