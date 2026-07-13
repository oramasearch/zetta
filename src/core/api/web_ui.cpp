#include "zetta/api/web_ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace zetta::api {

namespace {

// The whole design lives in one inline stylesheet (no external assets — air-gapped nodes have no
// egress). Orama design system: light surfaces, ink text, coral used surgically (the one brand dot),
// semantic green/amber/red status, Geist/Geist Mono with a system-font fallback. Mobile-first: the
// top bar wraps, the nav scrolls horizontally, content is one full-width column, tables scroll.
constexpr std::string_view kCss = R"CSS(
:root{
  --coral:#E2545A; --coral-wash:rgba(226,84,90,.06);
  --ink-primary:#1C1C1A; --ink-secondary:#444441; --ink-muted:#5F5F5C; --ink-quiet:#7D7D7A;
  --white:#FFFFFF; --canvas:#FAFAF8; --sidebar:#F5F4F2; --border:#E0E0E0;
  --rule-hair:rgba(0,0,0,.06); --rule-card:rgba(0,0,0,.08); --rule-emph:rgba(0,0,0,.12);
  --wash-hover:rgba(0,0,0,.04); --wash-selected:rgba(0,0,0,.05);
  --ok-fg:#0F6E56; --ok-bg:#E1F5EE; --warn-fg:#854F0B; --warn-bg:#FAEEDA; --err-fg:#B42318; --err-bg:#FBE9E8;
  --dot-active:#1D9E75; --dot-inactive:#888780;
  --font-sans:'Geist',-apple-system,BlinkMacSystemFont,'Segoe UI','Helvetica Neue',Arial,sans-serif;
  --font-mono:'Geist Mono',ui-monospace,SFMono-Regular,'SF Mono',Menlo,Consolas,monospace;
}
*{box-sizing:border-box;}
html,body{margin:0;}
body{background:var(--canvas);color:var(--ink-primary);font:15px/1.55 var(--font-sans);
  -webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale;}
.mono,code,pre{font-family:var(--font-mono);font-variant-numeric:tabular-nums;}
.q{color:var(--ink-quiet);}
a{color:var(--ink-secondary);text-decoration:none;}

.top{display:flex;align-items:center;flex-wrap:wrap;gap:8px 14px;padding:10px 16px;background:var(--white);
  border-bottom:1px solid var(--rule-hair);position:sticky;top:0;z-index:10;}
.brand{display:flex;align-items:center;gap:8px;font-size:16px;font-weight:500;letter-spacing:-.01em;color:var(--ink-primary);}
.brand .live{width:8px;height:8px;border-radius:50%;background:var(--coral);flex:none;}
.brand.big{font-size:21px;}
.spacer{flex:1;min-width:8px;}
.meta{font:11px/1.3 var(--font-mono);color:var(--ink-quiet);}
.user{font-size:12px;color:var(--ink-muted);}
.connpill{display:inline-flex;align-items:center;font:11px/1 var(--font-mono);font-weight:500;text-transform:uppercase;
  letter-spacing:.04em;padding:4px 8px;border-radius:8px;background:var(--wash-hover);color:var(--ink-quiet);}
.connpill.ok{color:var(--ok-fg);background:var(--ok-bg);}
form.inline{display:inline;margin:0;}

.nav{display:flex;gap:2px;padding:6px 10px;background:var(--sidebar);border-bottom:1px solid var(--rule-hair);
  overflow-x:auto;-webkit-overflow-scrolling:touch;position:sticky;top:52px;z-index:9;}
.nav a{white-space:nowrap;padding:7px 12px;border-radius:8px;color:var(--ink-secondary);font-size:13px;}
.nav a:hover{background:var(--wash-hover);}
.nav a.active{background:var(--white);color:var(--ink-primary);font-weight:500;}

.main{max-width:1160px;margin:0 auto;padding:18px 16px 56px;}
.panel{background:var(--white);border:1px solid var(--rule-card);border-radius:12px;padding:16px;margin-bottom:16px;}
.phead{display:flex;align-items:baseline;gap:10px;margin-bottom:12px;flex-wrap:wrap;}
.phead h2{margin:0;font-size:15px;font-weight:500;}
.psub{font-size:11px;color:var(--ink-muted);}
.empty{color:var(--ink-quiet);font-size:13px;padding:6px 0;}
.err-box{color:var(--err-fg);background:var(--err-bg);border-radius:8px;padding:10px 12px;font:12px/1.5 var(--font-mono);margin-bottom:12px;}

.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(138px,1fr));gap:12px;}
.card{background:var(--canvas);border:1px solid var(--rule-hair);border-radius:12px;padding:14px;}
.card .num{font:26px/1 var(--font-mono);color:var(--ink-primary);}
.card .lbl{margin-top:6px;font-size:11px;font-weight:500;text-transform:uppercase;letter-spacing:.04em;color:var(--ink-muted);}
.metaline{margin-top:12px;font-size:11px;}

.twrap{overflow-x:auto;-webkit-overflow-scrolling:touch;}
table{width:100%;border-collapse:collapse;font-size:13px;}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--rule-hair);vertical-align:top;white-space:nowrap;}
th{font:11px/1 var(--font-sans);font-weight:500;text-transform:uppercase;letter-spacing:.04em;color:var(--ink-muted);}
td{color:var(--ink-secondary);}
tbody tr:hover{background:var(--wash-hover);}
td .mono,td.mono{font-family:var(--font-mono);color:var(--ink-primary);font-size:12px;}
code.j{font-size:11px;color:var(--ink-muted);}

.pill{display:inline-flex;align-items:center;font:11px/1 var(--font-mono);font-weight:500;text-transform:uppercase;
  letter-spacing:.04em;border-radius:8px;padding:3px 7px;color:var(--ink-secondary);background:var(--wash-hover);}
.pill.ok{color:var(--ok-fg);background:var(--ok-bg);} .pill.warn{color:var(--warn-fg);background:var(--warn-bg);}
.pill.err{color:var(--err-fg);background:var(--err-bg);}
.status{display:inline-flex;align-items:center;gap:6px;color:var(--ink-secondary);}
.dot{width:8px;height:8px;border-radius:50%;background:var(--dot-inactive);}
.status.ok .dot{background:var(--dot-active);}

.btn{font:14px/1 var(--font-sans);color:var(--ink-primary);background:var(--wash-hover);border:1px solid var(--rule-emph);
  border-radius:8px;padding:9px 14px;cursor:pointer;}
.btn:hover{background:var(--wash-selected);}
.btn.small{padding:6px 10px;font-size:12px;}
.btn-primary{background:var(--ink-primary);color:var(--white);border-color:var(--ink-primary);}
.btn-primary:hover{background:#000;}
input,textarea,select{font:14px var(--font-sans);padding:9px 11px;border:1px solid var(--rule-emph);border-radius:8px;
  background:var(--canvas);color:var(--ink-primary);width:100%;}
textarea{font-family:var(--font-mono);font-size:12px;min-height:92px;resize:vertical;}
label{display:block;margin:9px 0 4px;font-size:12px;color:var(--ink-muted);}
pre{margin:8px 0 0;padding:12px;white-space:pre-wrap;word-break:break-word;font-size:12px;line-height:1.5;
  color:var(--ink-primary);background:var(--canvas);border:1px solid var(--rule-hair);border-radius:8px;overflow-x:auto;}

.loginwrap{min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
.logincard{background:var(--white);border:1px solid var(--rule-emph);border-radius:14px;padding:26px;
  width:min(400px,94vw);display:flex;flex-direction:column;gap:14px;}
.logincard .sub{margin:0;font-size:12px;color:var(--ink-muted);}
)CSS";

struct NavItem { std::string_view key; std::string_view label; };
constexpr std::array<NavItem, 9> kNav = {{
    {"overview", "Overview"}, {"cluster", "Cluster"}, {"data", "Data"}, {"models", "Models"},
    {"pipeline", "Pipeline"}, {"observability", "Observability"}, {"governance", "Governance"},
    {"sql", "SQL"}, {"console", "Console"}}};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string head(std::string_view title) {
    std::string out =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>";
    out += web_ui_escape(title);
    out += "</title><style>";
    out += kCss;
    out += "</style></head><body>";
    return out;
}

std::string tone_of(std::string value) {
    value = lower(std::move(value));
    auto has = [&](std::initializer_list<const char*> subs) {
        for (const char* s : subs)
            if (value.find(s) != std::string::npos) return true;
        return false;
    };
    if (has({"fail", "error", "deny", "expired", "revoked", "crash", "false",
             "degraded", "unreachable", "behind", "blocked"}))
        return "err";
    if (has({"syncing", "pending", "progress", "stale", "flight", "warn",
             "queued", "starting", "uncertain", "busy"}))
        return "warn";
    if (has({"live", "running", "ready", "pass", "ok", "approved", "healthy",
             "active", "exposed", "executed", "done", "complete", "succeed",
             "in sync", "in-sync", "insync", "true", "yes"}))
        return "ok";
    return "";
}

std::string human_bytes(double n) {
    static constexpr std::string_view unit[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int i = 0;
    while (n >= 1024.0 && i < 5) { n /= 1024.0; ++i; }
    std::ostringstream os;
    if (i == 0)
        os << static_cast<long long>(n);
    else
        os << std::fixed << std::setprecision(1) << n;
    os << ' ' << unit[i];
    return os.str();
}

bool contains_any(const std::string& col, std::initializer_list<const char*> subs) {
    for (const char* s : subs)
        if (col.find(s) != std::string::npos) return true;
    return false;
}

std::string fmt_cell(const std::string& column, const nlohmann::json& value) {
    if (value.is_null()) return "<span class=\"q\">&middot;</span>";
    const std::string col = lower(column);

    if (col == "status" || col == "verdict" || col == "outcome" || col == "phase" ||
        col == "reachability" || col == "state") {
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            return "<span class=\"pill " + tone_of(s) + "\">" + web_ui_escape(s) + "</span>";
        }
    }
    if (value.is_boolean()) {
        return value.get<bool>()
                   ? "<span class=\"status ok\"><span class=\"dot\"></span>yes</span>"
                   : "<span class=\"status\"><span class=\"dot\"></span>no</span>";
    }
    if (value.is_number()) {
        const bool bytes = contains_any(col, {"bytes", "_size", "vram"}) || col == "size";
        if (bytes) return "<span class=\"mono\">" + human_bytes(value.get<double>()) + "</span>";
        return "<span class=\"mono\">" + value.dump() + "</span>";
    }
    if (value.is_array()) {
        if (value.empty()) return "<span class=\"q\">&middot;</span>";
        std::string s;
        std::size_t n = 0;
        for (const auto& e : value) {
            if (n >= 6) { s += " +" + std::to_string(value.size() - 6); break; }
            if (n) s += ", ";
            s += e.is_string() ? e.get<std::string>() : e.dump();
            ++n;
        }
        return "<span class=\"mono\">" + web_ui_escape(s) + "</span>";
    }
    if (value.is_object()) {
        std::string s = value.dump();
        if (s.size() > 140) s = s.substr(0, 140) + "\xE2\x80\xA6";
        return "<code class=\"j\">" + web_ui_escape(s) + "</code>";
    }
    const std::string s = value.get<std::string>();
    if (s.empty()) return "<span class=\"q\">&middot;</span>";
    if (contains_any(col, {"_id", "node", "address", "path", "sha256", "endpoint", "version",
                           "hash", "trace", "span", "ref", "token", "created", "updated", "_at",
                           "owner", "columns", "fields", "scope", "permissions", "roles"}) ||
        col == "id")
        return "<span class=\"mono\">" + web_ui_escape(s) + "</span>";
    return web_ui_escape(s);
}

}  // namespace

std::string web_ui_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string web_ui_login(std::string_view error) {
    std::string out = head("Orama Zetta \xE2\x80\x94 sign in");
    out += "<div class=\"loginwrap\"><form class=\"logincard\" method=\"post\" action=\"/login\">";
    out += "<div class=\"brand big\"><span class=\"live\"></span>Orama Zetta</div>";
    out += "<p class=\"sub\">Sign in with an admin bearer token. It is kept only as an HttpOnly "
           "session cookie for this browser and validated on every request.</p>";
    if (!error.empty()) out += "<div class=\"err-box\">" + web_ui_escape(error) + "</div>";
    out += "<input type=\"password\" name=\"token\" placeholder=\"bearer token\" autocomplete=\"off\" autofocus>";
    out += "<button class=\"btn btn-primary\" type=\"submit\">Sign in</button>";
    out += "</form></div></body></html>";
    return out;
}

std::string web_ui_shell(std::string_view active, std::string_view user, std::string_view meta,
                         std::string_view content) {
    std::string out = head("Orama Zetta");
    out += "<div class=\"top\"><div class=\"brand\"><span class=\"live\"></span>Orama Zetta</div>";
    out += "<span class=\"connpill ok\">connected</span><span class=\"spacer\"></span>";
    out += "<span class=\"meta\">" + web_ui_escape(meta) + "</span>";
    if (!user.empty()) out += "<span class=\"user\">" + web_ui_escape(user) + "</span>";
    out += "<form class=\"inline\" method=\"post\" action=\"/logout\"><button class=\"btn small\">Sign out</button></form></div>";
    out += "<nav class=\"nav\">";
    for (const auto& item : kNav) {
        const bool on = item.key == active;
        out += "<a class=\"";
        out += on ? "active" : "";
        out += "\" href=\"/?view=";
        out += item.key;
        out += "\">";
        out += item.label;
        out += "</a>";
    }
    out += "</nav><main class=\"main\">";
    out += content;
    out += "</main></body></html>";
    return out;
}

std::string web_ui_panel(std::string_view title, std::string_view sub, std::string_view body) {
    std::string out = "<section class=\"panel\"><div class=\"phead\"><h2>" + web_ui_escape(title) + "</h2>";
    if (!sub.empty()) out += "<span class=\"psub\">" + web_ui_escape(sub) + "</span>";
    out += "</div>";
    out += body;
    out += "</section>";
    return out;
}

std::string web_ui_table(const nlohmann::json& columns, const nlohmann::json& rows) {
    if (!rows.is_array() || rows.empty()) return "<div class=\"empty\">No rows.</div>";
    std::vector<std::string> cols;
    if (columns.is_array() && !columns.empty()) {
        for (const auto& c : columns)
            if (c.is_string()) cols.push_back(c.get<std::string>());
    } else if (rows[0].is_object()) {
        for (auto it = rows[0].begin(); it != rows[0].end(); ++it) cols.push_back(it.key());
    }
    std::string out = "<div class=\"twrap\"><table><thead><tr>";
    for (const auto& c : cols) {
        std::string label = c;
        std::replace(label.begin(), label.end(), '_', ' ');
        out += "<th>" + web_ui_escape(label) + "</th>";
    }
    out += "</tr></thead><tbody>";
    for (const auto& row : rows) {
        out += "<tr>";
        for (const auto& c : cols) {
            const nlohmann::json& cell = (row.is_object() && row.contains(c)) ? row.at(c)
                                                                              : nlohmann::json();
            out += "<td>" + fmt_cell(c, cell) + "</td>";
        }
        out += "</tr>";
    }
    out += "</tbody></table></div>";
    return out;
}

std::string web_ui_overview_cards(const nlohmann::json& info) {
    auto count = [&](const char* key) -> std::string {
        if (info.contains(key) && info[key].is_number())
            return std::to_string(info[key].get<long long>());
        return "&middot;";
    };
    long long nodes = (info.contains("cluster_members") && info["cluster_members"].is_array())
                          ? static_cast<long long>(info["cluster_members"].size())
                          : 0;
    struct Card { const char* label; std::string value; };
    const std::array<Card, 6> cards = {{{"Nodes", std::to_string(nodes)},
                                        {"Databases", count("databases")},
                                        {"Projects", count("projects")},
                                        {"Models", count("models")},
                                        {"Runs", count("runs")},
                                        {"Jobs", count("jobs")}}};
    std::string out = "<div class=\"cards\">";
    for (const auto& c : cards)
        out += "<div class=\"card\"><div class=\"num\">" + c.value + "</div><div class=\"lbl\">" +
               c.label + "</div></div>";
    out += "</div>";
    return out;
}

}  // namespace zetta::api
