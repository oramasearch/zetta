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

.layout{display:grid;grid-template-columns:212px 1fr;align-items:start;}
.side{background:var(--sidebar);border-right:1px solid var(--rule-hair);padding:12px 8px;
  position:sticky;top:52px;align-self:stretch;min-height:calc(100vh - 52px);}
.side .group{font:11px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.08em;color:var(--ink-quiet);
  padding:14px 10px 6px;}
.side a{display:block;padding:8px 10px;margin-bottom:1px;border-radius:8px;color:var(--ink-secondary);
  font-size:13px;white-space:nowrap;}
.side a:hover{background:var(--wash-hover);}
.side a.active{background:var(--white);color:var(--ink-primary);font-weight:500;}
.main{padding:20px 24px 56px;max-width:1080px;min-width:0;}

/* Web shell — a terminal surface (charcoal), the one deliberately dark panel */
.term{background:#221F1D;color:#ECEAE6;border-radius:10px;padding:14px 16px;font:13px/1.55 var(--font-mono);
  overflow-x:auto;}
.term .prompt{color:var(--coral);}
.term .out{margin-top:6px;color:#D6D4CF;overflow-x:auto;}
.out .txt{margin:0;padding:0;background:none;border:none;white-space:pre;font:12px/1.55 var(--font-mono);color:inherit;}
.out .err{color:#ff9b9b;}
.out .rtab{border-collapse:collapse;font:12px/1.5 var(--font-mono);}
.out .rtab th{text-align:left;padding:2px 18px 5px 0;color:#948f88;font-weight:400;white-space:nowrap;border-bottom:1px solid rgba(255,255,255,.16);}
.out .rtab td{text-align:left;padding:2px 18px 2px 0;color:inherit;white-space:nowrap;}
.out .rcount{color:#948f88;font-size:11px;margin-top:8px;}
.term .cmd{color:#ECEAE6;}
.shellrow{display:flex;gap:8px;margin-top:12px;}
.shellrow input{font-family:var(--font-mono);}

/* Pagination + capability action forms + notebooks */
.pager{display:flex;gap:12px;align-items:center;margin-top:10px;font:11px/1 var(--font-mono);color:var(--ink-quiet);}
.pager a{color:var(--ink-secondary);border-bottom:1px dotted var(--rule-emph);}
.actionsub{font-size:11px;color:var(--ink-muted);}
.nbbar{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px;align-items:center;}
.nbbar select,.nbbar input{width:auto;}
.cell{border:1px solid var(--rule-card);border-radius:10px;padding:10px;margin-bottom:12px;}
.cellbar{display:flex;align-items:center;gap:8px;margin-bottom:8px;}
.cell textarea{min-height:60px;}
.cell .out{margin-top:8px;background:#221F1D;color:#ECEAE6;border-radius:8px;padding:12px;overflow-x:auto;}
.cell .out:empty{display:none;}

@media (max-width:760px){
  .layout{grid-template-columns:1fr;}
  .side{position:sticky;top:52px;min-height:0;display:flex;gap:2px;overflow-x:auto;
    -webkit-overflow-scrolling:touch;border-right:none;border-bottom:1px solid var(--rule-hair);padding:6px 10px;z-index:9;}
  .side .group{display:none;}
  .main{padding:16px;}
}
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

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string head(std::string_view title) {
    std::string out =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"icon\" href=\"/favicon.svg\" type=\"image/svg+xml\">"
        "<link rel=\"stylesheet\" href=\"/style.css\">"
        "<title>";
    out += web_ui_escape(title);
    out += "</title></head><body>";
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
    auto link = [&](std::string_view key, std::string_view label) -> std::string {
        std::string anchor = "<a class=\"";
        if (key == active) anchor += "active";
        anchor += "\" href=\"/?view=";
        anchor += key;
        anchor += "\">";
        anchor += label;
        anchor += "</a>";
        return anchor;
    };
    std::string out = head("Orama Zetta");
    out += "<div class=\"top\"><div class=\"brand\"><span class=\"live\"></span>Orama Zetta</div>";
    out += "<span class=\"connpill ok\">connected</span><span class=\"spacer\"></span>";
    out += "<span class=\"meta\">" + web_ui_escape(meta) + "</span>";
    if (!user.empty()) out += "<span class=\"user\">" + web_ui_escape(user) + "</span>";
    out += "<form class=\"inline\" method=\"post\" action=\"/logout\"><button class=\"btn small\">Sign out</button></form></div>";
    out += "<div class=\"layout\"><nav class=\"side\">";
    out += "<div class=\"group\">Cluster</div>" + link("overview", "Overview") + link("cluster", "Cluster");
    out += "<div class=\"group\">Workloads</div>" + link("data", "Data") + link("models", "Models") +
           link("pipeline", "Pipeline");
    out += "<div class=\"group\">Operate</div>" + link("observability", "Observability") +
           link("governance", "Governance");
    out += "<div class=\"group\">Build</div>" + link("shell", "Shell") + link("notebooks", "Notebooks");
    out += "</nav><main class=\"main\">";
    out += content;
    out += "</main></div></body></html>";
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

std::string web_ui_stylesheet() { return std::string(kCss); }

std::string web_ui_favicon() {
    return R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><rect width="32" height="32" rx="7" fill="#E2545A"/><path d="M9 9 H23 V12 L14 20 H23 V23 H9 V20 L18 12 H9 Z" fill="#fff"/></svg>)SVG";
}

namespace {
// Shared client helpers for the interactive tools (Shell + Notebooks): POST a command to the
// cookie-authed /shell endpoint and format a SQL result as a monospace text table.
constexpr std::string_view kSharedShellJs = R"JS(
function zEsc(s){return String(s==null?'':s).replace(/[&<>]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
async function zExec(q){var r=await fetch('/shell',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'q='+encodeURIComponent(q)});return r.json();}
function zFmt(r){
  if(!r||r.error) return '<pre class="txt err">error: '+zEsc((r&&r.error)?r.error:'request failed')+'</pre>';
  if(!r.columns) return '<pre class="txt">'+zEsc(JSON.stringify(r.value!==undefined?r.value:r,null,2))+'</pre>';
  var cols=r.columns,rows=r.rows||[];
  if(!rows.length) return '<div class="rcount">(0 rows)</div>';
  var h='<table class="rtab"><thead><tr>';
  cols.forEach(function(c){h+='<th>'+zEsc(c)+'</th>';});
  h+='</tr></thead><tbody>';
  rows.forEach(function(row){h+='<tr>';cols.forEach(function(c){var v=row[c];v=(v==null?'·':(typeof v==='object'?JSON.stringify(v):String(v)));h+='<td>'+zEsc(v)+'</td>';});h+='</tr>';});
  return h+'</tbody></table><div class="rcount">('+rows.length+' row'+(rows.length===1?'':'s')+')</div>';
}
)JS";
}  // namespace

std::string web_ui_shell_repl() {
    std::string out =
        "<section class=\"panel\"><div class=\"phead\"><h2>Shell</h2>"
        "<span class=\"psub\">SQL REPL over the read model \xC2\xB7 Enter runs \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 history</span></div>"
        "<div class=\"term\" id=\"term\"><div class=\"q\">Orama Zetta shell \xC2\xB7 type SQL, or <b>help</b>.</div></div>"
        "<div class=\"shellrow\"><input id=\"cmd\" placeholder=\"SELECT * FROM system.nodes\" autocomplete=\"off\" spellcheck=\"false\" autofocus>"
        "<button class=\"btn btn-primary\" id=\"run\">Run</button></div></section><script>";
    out += kSharedShellJs;
    out += R"JS(
(function(){
  var term=document.getElementById('term'),cmd=document.getElementById('cmd'),run=document.getElementById('run'),hist=[],hi=-1;
  var HELP='Orama Zetta shell — a SQL REPL over the read model (the ~30 system.* views).\n\n'
    +'  <SQL>        run a statement, e.g. SELECT * FROM system.nodes\n'
    +'  \\tables      list the system.* tables\n'
    +'  \\d <table>   show a table (columns + rows)\n'
    +'  help  or  ?  this help\n'
    +'  clear        clear the screen\n\n'
    +'Up / Down recall history.';
  function meta(c){
    var l=c.toLowerCase();
    if(l==='help'||l==='\\help'||l==='.help'||l==='?') return {text:HELP};
    if(l==='clear'||l==='\\clear'||l==='.clear'||l==='cls') return {clear:true};
    if(l==='\\tables'||l==='.tables'||l==='\\dt') return {sql:'SELECT * FROM system.tables'};
    if(l.indexOf('\\d ')===0||l.indexOf('.d ')===0) return {sql:'SELECT * FROM system.'+c.slice(3).trim().replace(/^system\./i,'')};
    return null;
  }
  async function go(){
    var c=cmd.value.trim(); if(!c) return;
    hist.push(c); hi=hist.length; cmd.value='';
    var m=meta(c);
    if(m&&m.clear){ term.innerHTML=''; return; }
    var b=document.createElement('div');
    b.innerHTML='<div><span class="prompt">zetta&gt;</span> <span class="cmd">'+zEsc(c)+'</span></div><div class="out"></div>';
    term.appendChild(b); var o=b.querySelector('.out');
    if(m&&m.text){ o.innerHTML='<pre class="txt">'+zEsc(m.text)+'</pre>'; term.scrollTop=term.scrollHeight; return; }
    o.innerHTML='<span class="txt">running…</span>'; term.scrollTop=term.scrollHeight;
    try{ o.innerHTML=zFmt(await zExec((m&&m.sql)?m.sql:c)); }catch(e){ o.innerHTML='<pre class="txt err">error: '+zEsc(''+e)+'</pre>'; }
    term.scrollTop=term.scrollHeight;
  }
  run.onclick=go;
  cmd.addEventListener('keydown',function(e){
    if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();go();}
    else if(e.key==='ArrowUp'){if(hi>0){hi--;cmd.value=hist[hi];}e.preventDefault();}
    else if(e.key==='ArrowDown'){if(hi<hist.length-1){hi++;cmd.value=hist[hi];}else{hi=hist.length;cmd.value='';}e.preventDefault();}
  });
})();
)JS";
    out += "</script>";
    return out;
}

std::string web_ui_notebooks() {
    std::string out =
        "<section class=\"panel\"><div class=\"phead\"><h2>Notebooks</h2>"
        "<span class=\"psub\">SQL cells \xC2\xB7 Cmd/Ctrl+Enter runs a cell \xC2\xB7 saved in this browser</span></div>"
        "<div class=\"nbbar\"><select id=\"nbsel\"></select><input id=\"nbname\" placeholder=\"name\">"
        "<button class=\"btn small\" id=\"nbnew\">New</button><button class=\"btn small\" id=\"nbsave\">Save</button>"
        "<button class=\"btn small\" id=\"nbdel\">Delete</button><button class=\"btn small\" id=\"nbadd\">+ Cell</button></div>"
        "<div id=\"cells\"></div></section><script>";
    out += kSharedShellJs;
    out += R"JS(
(function(){
  var KEY='zetta_notebooks',cellsEl=document.getElementById('cells'),sel=document.getElementById('nbsel'),name=document.getElementById('nbname');
  function load(){try{return JSON.parse(localStorage.getItem(KEY))||{};}catch(e){return {};}}
  function save(nb){localStorage.setItem(KEY,JSON.stringify(nb));}
  var book=[{src:'SELECT * FROM system.nodes',out:''}];
  async function runCell(i,d){var o=d.querySelector('.out');o.innerHTML='<span class="txt">running…</span>';try{book[i].out=zFmt(await zExec(book[i].src));o.innerHTML=book[i].out;}catch(e){o.innerHTML='<pre class="txt err">error: '+zEsc(''+e)+'</pre>';book[i].out='';}}
  function render(){
    cellsEl.innerHTML='';
    book.forEach(function(cell,i){
      var d=document.createElement('div');d.className='cell';
      d.innerHTML='<div class="cellbar"><span class="q mono">['+(i+1)+']</span><button class="btn small run">Run</button><button class="btn small del">✕</button></div><textarea class="src" spellcheck="false"></textarea><div class="out">'+(cell.out||'')+'</div>';
      var ta=d.querySelector('.src');ta.value=cell.src;
      ta.addEventListener('input',function(){cell.src=this.value;});
      ta.addEventListener('keydown',function(e){if((e.metaKey||e.ctrlKey)&&e.key==='Enter'){e.preventDefault();runCell(i,d);}});
      d.querySelector('.run').onclick=function(){runCell(i,d);};
      d.querySelector('.del').onclick=function(){book.splice(i,1);if(!book.length)book.push({src:'',out:''});render();};
      cellsEl.appendChild(d);
    });
  }
  function refreshSel(){var nb=load();sel.innerHTML='<option value="">— open —</option>'+Object.keys(nb).map(function(n){return '<option>'+zEsc(n)+'</option>';}).join('');}
  document.getElementById('nbadd').onclick=function(){book.push({src:'',out:''});render();};
  document.getElementById('nbnew').onclick=function(){book=[{src:'',out:''}];name.value='';render();};
  document.getElementById('nbsave').onclick=function(){var n=(name.value||'untitled').trim();var nb=load();nb[n]=book;save(nb);refreshSel();sel.value=n;};
  document.getElementById('nbdel').onclick=function(){var n=name.value.trim();if(!n)return;var nb=load();delete nb[n];save(nb);refreshSel();};
  sel.onchange=function(){var n=sel.value;if(!n)return;book=JSON.parse(JSON.stringify(load()[n]||[]));name.value=n;render();};
  refreshSel();render();
})();
)JS";
    out += "</script>";
    return out;
}

}  // namespace zetta::api
