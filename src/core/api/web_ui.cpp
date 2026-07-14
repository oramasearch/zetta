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
// egress). Orama design system: light surfaces, ink text, a cool accent blue used surgically (the one
// brand dot / active icon / focus ring — red is reserved for alarms only), semantic green/amber/red
// status, Geist/Geist Mono with a system-font fallback. Mobile-first: the
// top bar wraps, the nav scrolls horizontally, content is one full-width column, tables scroll.
constexpr std::string_view kCss = R"CSS(
:root{
  --accent:#2563EB; --accent-wash:rgba(37,99,235,.09);
  --ink-primary:#1C1C1A; --ink-secondary:#444441; --ink-muted:#5F5F5C; --ink-quiet:#7D7D7A;
  --white:#FFFFFF; --canvas:#FAFAF8; --sidebar:#F5F4F2; --border:#E0E0E0;
  --rule-hair:rgba(0,0,0,.06); --rule-card:rgba(0,0,0,.08); --rule-emph:rgba(0,0,0,.12);
  --wash-hover:rgba(0,0,0,.04); --wash-selected:rgba(0,0,0,.05);
  --ok-fg:#0F6E56; --ok-bg:#E1F5EE; --warn-fg:#854F0B; --warn-bg:#FAEEDA; --err-fg:#B42318; --err-bg:#FBE9E8;
  --info-fg:#3457B2; --info-bg:#EAEFFB;
  --dot-active:#1D9E75; --dot-inactive:#888780;
  --shadow-sm:0 1px 2px rgba(24,24,20,.04),0 1px 1px rgba(24,24,20,.03);
  --shadow-md:0 6px 22px rgba(24,24,20,.06),0 2px 6px rgba(24,24,20,.04);
  --shadow-pop:0 12px 36px rgba(24,24,20,.14);
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
.brand .live{width:8px;height:8px;border-radius:50%;background:var(--accent);flex:none;}
.brand.big{font-size:21px;}
.spacer{flex:1;min-width:8px;}
.meta{font:11px/1.3 var(--font-mono);color:var(--ink-quiet);}
.user{font-size:12px;color:var(--ink-muted);}
.connpill{display:inline-flex;align-items:center;font:11px/1 var(--font-mono);font-weight:500;text-transform:uppercase;
  letter-spacing:.04em;padding:4px 8px;border-radius:8px;background:var(--wash-hover);color:var(--ink-quiet);}
.connpill.ok{color:var(--ok-fg);background:var(--ok-bg);}
form.inline{display:inline;margin:0;}

.layout{display:grid;grid-template-columns:224px 1fr;align-items:start;}
.side{background:var(--sidebar);border-right:1px solid var(--rule-hair);padding:14px 10px;
  position:sticky;top:52px;align-self:stretch;min-height:calc(100vh - 52px);}
.side .group{font:10.5px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.09em;color:var(--ink-quiet);
  padding:16px 10px 7px;}
.side a{display:flex;align-items:center;gap:10px;padding:8px 10px;margin-bottom:2px;border-radius:9px;color:var(--ink-secondary);
  font-size:13.5px;white-space:nowrap;transition:background .12s,color .12s;}
.side a svg{width:16px;height:16px;flex:none;opacity:.72;}
.side a:hover{background:var(--wash-hover);color:var(--ink-primary);}
.side a.active{background:var(--white);color:var(--ink-primary);font-weight:500;box-shadow:var(--shadow-sm);}
.side a.active svg{opacity:1;color:var(--accent);}
.main{padding:24px 28px 60px;min-width:0;}  /* fill all available width — no max cap */

/* Web shell — a terminal surface (charcoal), the one deliberately dark panel */
.term{background:var(--canvas);color:var(--ink-primary);border:1px solid var(--rule-card);border-radius:12px;padding:20px 22px;font:14.5px/1.7 var(--font-mono);
  overflow-x:auto;}
.term>div{margin-bottom:18px;}
.term>div:last-child{margin-bottom:0;}
.term .prompt{color:var(--accent);margin-right:4px;}
.term .out{margin-top:10px;color:var(--ink-primary);overflow-x:auto;}
.out .txt{margin:0;padding:0;background:none;border:none;white-space:pre-wrap;font:14px/1.7 var(--font-mono);color:inherit;}
.out .err{color:var(--err-fg);}
.out .rtab{border-collapse:collapse;font:14px/1.7 var(--font-mono);margin-top:2px;}
.out .rtab th{text-align:left;padding:4px 26px 10px 0;color:var(--ink-muted);font-weight:500;white-space:nowrap;border-bottom:1px solid var(--rule-emph);}
.out .rtab td{text-align:left;padding:8px 26px 8px 0;color:var(--ink-secondary);white-space:nowrap;border-bottom:1px solid var(--rule-hair);}
.out .rtab tr:last-child td{border-bottom:none;}
.out .rcount{color:var(--ink-quiet);font-size:12.5px;margin-top:12px;}
.exectime{text-align:right;color:var(--ink-quiet);font:11px/1 var(--font-mono);margin-top:8px;}
.term .cmd{color:var(--ink-primary);white-space:pre-wrap;}
.shellrow{display:flex;gap:8px;margin-top:14px;position:relative;align-items:flex-end;}
.shellrow textarea{font:14.5px/1.6 var(--font-mono);padding:12px 14px;min-height:48px;max-height:240px;resize:none;overflow-y:auto;}
.acmenu{position:absolute;top:calc(100% + 6px);left:0;right:0;background:var(--white);border:1px solid var(--rule-emph);
  border-radius:11px;box-shadow:var(--shadow-pop);max-height:238px;overflow-y:auto;z-index:40;display:none;padding:6px;}
.acmenu.show{display:block;}
.acitem{display:flex;justify-content:space-between;align-items:baseline;gap:18px;padding:9px 11px;border-radius:8px;cursor:pointer;}
.acitem .acname{font:13.5px var(--font-mono);color:var(--ink-primary);white-space:nowrap;}
.acitem .acdesc{font:12px var(--font-sans);color:var(--ink-quiet);text-align:right;}
.acitem.sel,.acitem:hover{background:var(--accent-wash);}
.acitem.sel .acname{color:var(--accent);}

/* Pagination + capability action forms + notebooks */
.pager{display:flex;gap:12px;align-items:center;margin-top:12px;font:11.5px/1 var(--font-mono);color:var(--ink-quiet);}
.pager a{color:var(--ink-secondary);padding:5px 11px;border:1px solid var(--rule-emph);border-radius:7px;background:var(--white);}
.pager a:hover{background:var(--wash-hover);color:var(--ink-primary);}
.pager .pspacer{flex:1;}
.actionsub{font-size:11px;color:var(--ink-muted);}
.nbbar{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px;align-items:center;}
.nbbar select,.nbbar input{width:auto;}
.cell{border:1px solid var(--rule-card);border-radius:10px;padding:10px;margin-bottom:12px;}
.cellbar{display:flex;align-items:center;gap:8px;margin-bottom:8px;}
.cell textarea{min-height:60px;}
.cell .out{margin-top:8px;background:var(--canvas);color:var(--ink-primary);border:1px solid var(--rule-hair);border-radius:8px;padding:12px;overflow-x:auto;}
.cell .out:empty{display:none;}

@media (max-width:760px){
  .layout{grid-template-columns:1fr;}
  .side{position:sticky;top:52px;min-height:0;display:flex;gap:2px;overflow-x:auto;
    -webkit-overflow-scrolling:touch;border-right:none;border-bottom:1px solid var(--rule-hair);padding:6px 10px;z-index:9;}
  .side .group{display:none;}
  .main{padding:16px;}
}
.panel{background:var(--white);border:1px solid var(--rule-card);border-radius:14px;padding:18px 20px;margin-bottom:18px;box-shadow:var(--shadow-sm);}
.phead{display:flex;align-items:center;gap:11px;margin-bottom:15px;flex-wrap:wrap;}
.phead .ico{width:27px;height:27px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;
  background:var(--accent-wash);color:var(--accent);flex:none;}
.phead .ico svg{width:15px;height:15px;}
.phead h2{margin:0;font-size:15.5px;font-weight:600;letter-spacing:-.01em;color:var(--ink-primary);}
.phead .psub{font-size:11.5px;color:var(--ink-muted);}
.phead .hcount{margin-left:auto;font:11px/1 var(--font-mono);color:var(--ink-quiet);background:var(--wash-hover);
  padding:5px 10px;border-radius:20px;}
.eyebrow{font:10.5px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.09em;color:var(--ink-quiet);
  margin:28px 2px 13px;}
.empty{color:var(--ink-quiet);font-size:13px;padding:6px 0;}
.emptybox{display:flex;flex-direction:column;align-items:center;gap:9px;text-align:center;padding:32px 16px;color:var(--ink-quiet);}
.emptybox .eico{width:40px;height:40px;border-radius:11px;display:inline-flex;align-items:center;justify-content:center;
  background:var(--wash-hover);color:var(--ink-quiet);}
.emptybox .eico svg{width:20px;height:20px;}
.emptybox .et{font-size:13.5px;color:var(--ink-muted);font-weight:500;}
.emptybox .eh{font-size:12px;color:var(--ink-quiet);max-width:340px;}
.err-box{color:var(--err-fg);background:var(--err-bg);border:1px solid rgba(180,35,24,.14);border-radius:9px;padding:11px 13px;font:12px/1.5 var(--font-mono);margin-bottom:12px;}

.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));gap:14px;}
.card{background:var(--white);border:1px solid var(--rule-card);border-radius:13px;padding:16px;box-shadow:var(--shadow-sm);
  transition:box-shadow .14s ease,transform .14s ease;}
.card:hover{box-shadow:var(--shadow-md);transform:translateY(-1px);}
.card .chd{display:flex;align-items:center;justify-content:space-between;margin-bottom:13px;}
.card .cico{width:31px;height:31px;border-radius:9px;display:inline-flex;align-items:center;justify-content:center;
  background:var(--accent-wash);color:var(--accent);}
.card .cico svg{width:16px;height:16px;}
.card .num{font:29px/1 var(--font-mono);font-weight:500;color:var(--ink-primary);letter-spacing:-.02em;font-variant-numeric:tabular-nums;}
.card .lbl{margin-top:8px;font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.05em;color:var(--ink-muted);}
.metaline{margin-top:14px;font-size:11px;color:var(--ink-quiet);}

.twrap{overflow-x:auto;-webkit-overflow-scrolling:touch;border:1px solid var(--rule-hair);border-radius:11px;}
table{width:100%;border-collapse:collapse;font-size:13px;}
thead th{position:sticky;top:0;background:var(--canvas);z-index:1;}
th,td{text-align:left;padding:9px 13px;border-bottom:1px solid var(--rule-hair);vertical-align:top;white-space:nowrap;}
th{font:10.5px/1 var(--font-sans);font-weight:600;text-transform:uppercase;letter-spacing:.05em;color:var(--ink-muted);}
td{color:var(--ink-secondary);}
tbody tr:last-child td{border-bottom:none;}
tbody tr:hover{background:var(--wash-hover);}
td.num,th.num{text-align:right;font-variant-numeric:tabular-nums;}
td .mono,td.mono{font-family:var(--font-mono);color:var(--ink-primary);font-size:12px;}
code.j{font-size:11px;color:var(--ink-muted);}

/* Data explorer: databases | tables | rows (rows fills the remaining width) */
.explorer{display:grid;grid-template-columns:minmax(150px,180px) minmax(180px,230px) 1fr;
  border:1px solid var(--rule-hair);border-radius:11px;overflow:hidden;min-height:300px;}
.expcol{border-right:1px solid var(--rule-hair);min-width:0;display:flex;flex-direction:column;}
.expcol:last-child{border-right:none;}
.exphd{font:10.5px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.07em;color:var(--ink-quiet);
  padding:11px 13px;border-bottom:1px solid var(--rule-hair);background:var(--canvas);}
.expbody{overflow-y:auto;max-height:540px;}
.expbody-rows{padding:12px;}
.exprow{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:9px 13px;font-size:13px;
  color:var(--ink-secondary);border-bottom:1px solid var(--rule-hair);cursor:pointer;}
.exprow>span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.exprow:last-child{border-bottom:none;}
.exprow:hover{background:var(--wash-hover);color:var(--ink-primary);}
.exprow.on{background:var(--accent-wash);color:var(--accent);font-weight:500;}
.exprow .ct{font:11px/1 var(--font-mono);color:var(--ink-quiet);flex:none;}
.exprow.on .ct{color:var(--accent);}
.exphint{padding:16px 13px;color:var(--ink-quiet);font-size:12.5px;}
@media(max-width:820px){.explorer{grid-template-columns:1fr;}
  .expcol{border-right:none;border-bottom:1px solid var(--rule-hair);} .expbody{max-height:240px;}}

.pill{display:inline-flex;align-items:center;font:11px/1 var(--font-mono);font-weight:500;text-transform:uppercase;
  letter-spacing:.04em;border-radius:8px;padding:3px 7px;color:var(--ink-secondary);background:var(--wash-hover);}
.pill.ok{color:var(--ok-fg);background:var(--ok-bg);} .pill.warn{color:var(--warn-fg);background:var(--warn-bg);}
.pill.err{color:var(--err-fg);background:var(--err-bg);}
.status{display:inline-flex;align-items:center;gap:6px;color:var(--ink-secondary);}
.dot{width:8px;height:8px;border-radius:50%;background:var(--dot-inactive);}
.status.ok .dot{background:var(--dot-active);}

.btn{font:13.5px/1 var(--font-sans);font-weight:500;color:var(--ink-primary);background:var(--white);border:1px solid var(--rule-emph);
  border-radius:9px;padding:10px 15px;cursor:pointer;transition:background .12s,border-color .12s,box-shadow .12s;}
.btn:hover{background:var(--wash-hover);}
.btn.small{padding:6px 11px;font-size:12px;border-radius:8px;}
.btn-primary{display:inline-flex;align-items:center;gap:7px;background:var(--ink-primary);color:var(--white);border-color:var(--ink-primary);box-shadow:var(--shadow-sm);}
.btn-primary:hover{background:#000;}
.btn-primary svg{width:14px;height:14px;}
input,textarea,select{font:14px var(--font-sans);padding:10px 12px;border:1px solid var(--rule-emph);border-radius:9px;
  background:var(--canvas);color:var(--ink-primary);width:100%;transition:border-color .12s,box-shadow .12s,background .12s;}
input:focus,textarea:focus,select:focus{outline:none;border-color:var(--accent);background:var(--white);box-shadow:0 0 0 3px var(--accent-wash);}
input::placeholder{color:var(--ink-quiet);}
textarea{font-family:var(--font-mono);font-size:13px;min-height:92px;resize:vertical;}
label{display:block;margin:12px 0 5px;font-size:12px;font-weight:500;color:var(--ink-secondary);}
label .req{color:var(--accent);margin-left:2px;}
pre{margin:8px 0 0;padding:13px;white-space:pre-wrap;word-break:break-word;font-size:12px;line-height:1.55;
  color:var(--ink-primary);background:var(--canvas);border:1px solid var(--rule-hair);border-radius:9px;overflow-x:auto;}

/* Collapsible action forms (native details/summary) + 2-col field grid */
.action{border:1px solid var(--rule-card);border-radius:12px;margin-bottom:12px;background:var(--white);overflow:hidden;transition:box-shadow .14s;}
.action[open]{box-shadow:var(--shadow-sm);}
.action>summary{list-style:none;cursor:pointer;display:flex;align-items:center;gap:11px;padding:14px 16px;}
.action>summary::-webkit-details-marker{display:none;}
.action>summary .aico{width:28px;height:28px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;
  background:var(--wash-hover);color:var(--ink-muted);flex:none;transition:background .12s,color .12s;}
.action[open]>summary .aico,.action>summary:hover .aico{background:var(--accent-wash);color:var(--accent);}
.action>summary .aico svg{width:15px;height:15px;}
.action>summary .an{font:13px/1.2 var(--font-mono);font-weight:500;color:var(--ink-primary);}
.action>summary .ad{font-size:12px;color:var(--ink-quiet);flex:1;min-width:40px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.action>summary .chev{margin-left:auto;color:var(--ink-quiet);transition:transform .15s;flex:none;display:inline-flex;}
.action[open]>summary .chev{transform:rotate(90deg);}
.action .abody{padding:4px 16px 16px;border-top:1px solid var(--rule-hair);}
.action .abody>.adesc{font-size:12.5px;color:var(--ink-muted);margin:12px 0 2px;line-height:1.5;}
.fgrid{display:grid;grid-template-columns:1fr 1fr;gap:0 18px;}
.fgrid .full{grid-column:1/-1;}
@media (max-width:620px){.fgrid{grid-template-columns:1fr;}}

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

// FNV-1a of the stylesheet — stable per build, changes when the CSS changes. Used as the cache-busting
// ?v= on the /style.css + /favicon.svg links so a long Cache-Control never serves a stale stylesheet.
std::string asset_version() {
    unsigned long long hash = 1469598103934665603ULL;
    for (char c : kCss) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    std::ostringstream os;
    os << std::hex << (hash & 0xFFFFFFFULL);
    return os.str();
}

std::string head(std::string_view title) {
    static const std::string version = asset_version();
    std::string out =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<link rel=\"icon\" href=\"/favicon.svg?v=" + version + "\" type=\"image/svg+xml\">"
        "<link rel=\"stylesheet\" href=\"/style.css?v=" + version + "\">"
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

// A signed integer with thousands separators (1234567 -> "1,234,567").
std::string fmt_int(long long n) {
    const bool negative = n < 0;
    unsigned long long value =
        negative ? static_cast<unsigned long long>(-(n + 1)) + 1ULL : static_cast<unsigned long long>(n);
    const std::string digits = std::to_string(value);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count && count % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++count;
    }
    if (negative) out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

// A duration in seconds, humanized: 0.42 -> "420 ms", 90 -> "1m 30s", 3661 -> "1h 1m" (max 2 units).
std::string fmt_duration_seconds(double seconds) {
    if (seconds > 0 && seconds < 1.0) {
        std::ostringstream os;
        os << static_cast<long long>(seconds * 1000.0 + 0.5) << " ms";
        return os.str();
    }
    long long total = static_cast<long long>(seconds + (seconds < 0 ? -0.5 : 0.5));
    if (total == 0) return "0s";
    const bool negative = total < 0;
    if (negative) total = -total;
    const long long days = total / 86400; total %= 86400;
    const long long hours = total / 3600; total %= 3600;
    const long long minutes = total / 60; total %= 60;
    std::string out;
    int parts = 0;
    auto add = [&](long long v, const char* unit) {
        if (v && parts < 2) {
            if (!out.empty()) out += " ";
            out += std::to_string(v);
            out += unit;
            ++parts;
        }
    };
    add(days, "d"); add(hours, "h"); add(minutes, "m"); add(total, "s");
    if (out.empty()) out = "0s";
    return (negative ? "-" : "") + out;
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
        if (contains_any(col, {"bytes", "_size", "vram"}) || col == "size")
            return "<span class=\"mono\">" + human_bytes(value.get<double>()) + "</span>";
        if (contains_any(col, {"seconds", "elapsed", "duration", "uptime", "latency", "_ms"})) {
            double v = value.get<double>();
            if (col.find("_ms") != std::string::npos) v /= 1000.0;  // a *_ms column is milliseconds
            return "<span class=\"mono\">" + fmt_duration_seconds(v) + "</span>";
        }
        if (value.is_number_integer() || value.is_number_unsigned())
            return "<span class=\"mono\">" + fmt_int(value.get<long long>()) + "</span>";
        std::ostringstream os;
        os << std::setprecision(4) << value.get<double>();  // trim trailing-zero float noise
        return "<span class=\"mono\">" + os.str() + "</span>";
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

std::string web_ui_icon(std::string_view name) {
    // 24x24 line icons; currentColor stroke, sized by the CSS at the use site.
    auto svg = [](std::string_view body) {
        return "<svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" "
               "stroke-linecap=\"round\" stroke-linejoin=\"round\">" +
               std::string(body) + "</svg>";
    };
    if (name == "overview")
        return svg("<rect x='3' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='3' width='7' height='7' rx='1.5'/>"
                   "<rect x='3' y='14' width='7' height='7' rx='1.5'/><rect x='14' y='14' width='7' height='7' rx='1.5'/>");
    if (name == "cluster")
        return svg("<circle cx='12' cy='5' r='2.3'/><circle cx='5' cy='19' r='2.3'/><circle cx='19' cy='19' r='2.3'/>"
                   "<path d='M11 6.9 6 16.9M13 6.9l5 10M7.3 19h9.4'/>");
    if (name == "data")
        return svg("<ellipse cx='12' cy='6' rx='7.5' ry='3'/><path d='M4.5 6v12c0 1.66 3.36 3 7.5 3s7.5-1.34 7.5-3V6'/>"
                   "<path d='M4.5 12c0 1.66 3.36 3 7.5 3s7.5-1.34 7.5-3'/>");
    if (name == "models")
        return svg("<path d='M12 2.5 20.5 7v10L12 21.5 3.5 17V7z'/><path d='M12 2.5v19M3.5 7l8.5 4.6 8.5-4.6'/>");
    if (name == "pipeline")
        return svg("<circle cx='6' cy='6' r='2.1'/><circle cx='6' cy='18' r='2.1'/><circle cx='18' cy='12' r='2.1'/>"
                   "<path d='M6 8.1v7.8M8 6h4.5a3 3 0 0 1 3 3v.9M8 18h4.5a3 3 0 0 0 3-3v-.9'/>");
    if (name == "observability") return svg("<path d='M3 12h4l2.5-7 5 14 2.5-7H21'/>");
    if (name == "governance")
        return svg("<path d='M12 2.5 20 6v6c0 5-3.4 8-8 9.5C8 20 4.6 17 4.6 12V6z'/><path d='m9 12 2.2 2.2L15.5 10'/>");
    if (name == "shell")
        return svg("<rect x='3' y='4' width='18' height='16' rx='2.2'/><path d='m7.5 9 3 3-3 3M13 15.5h4'/>");
    if (name == "notebooks")
        return svg("<path d='M6 3.5h11a1.5 1.5 0 0 1 1.5 1.5v15.5H6z'/><path d='M6 3.5A2.5 2.5 0 0 0 3.5 6v12A2.5 2.5 0 0 1 6 15.5h12.5'/><path d='M9.5 8h5'/>");
    if (name == "nodes")
        return svg("<rect x='3' y='4' width='18' height='7' rx='1.6'/><rect x='3' y='13' width='18' height='7' rx='1.6'/><path d='M6.5 7.5h.01M6.5 16.5h.01'/>");
    if (name == "projects")
        return svg("<path d='M3 7a2 2 0 0 1 2-2h3.6l2 2.2H19a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z'/>");
    if (name == "runs") return svg("<circle cx='12' cy='12' r='9'/><path d='m10 8.3 5.2 3.7-5.2 3.7z'/>");
    if (name == "jobs")
        return svg("<rect x='5' y='5' width='14' height='14' rx='2.4'/><path d='M9 2.5v2.5M15 2.5v2.5M9 19v2.5M15 19v2.5"
                   "M2.5 9H5M2.5 15H5M19 9h2.5M19 15h2.5'/>");
    if (name == "inbox")
        return svg("<path d='M3 13.5 6 5.7A2 2 0 0 1 7.9 4.5h8.2A2 2 0 0 1 18 5.7l3 7.8'/>"
                   "<path d='M3 13.5V18a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-4.5h-6a3 3 0 0 1-6 0z'/>");
    if (name == "bolt") return svg("<path d='M13 2.5 4.5 13.5H11l-1 8 8.5-11H12z'/>");
    if (name == "chevron") return svg("<path d='m9 6 6 6-6 6'/>");
    if (name == "play") return svg("<path d='m7 4.5 12 7.5-12 7.5z'/>");
    return svg("");
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
        anchor += web_ui_icon(key);  // nav keys map 1:1 to icon names
        anchor += "<span>";
        anchor += label;
        anchor += "</span></a>";
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

// Pick a section icon from the panel title's keywords (keeps the panel signature unchanged).
std::string panel_icon_for(std::string_view title) {
    const std::string t = lower(std::string(title));
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    if (has("dataset") || has("data") || has("index")) return web_ui_icon("data");
    if (has("node") || has("cluster") || has("member")) return web_ui_icon("cluster");
    if (has("model")) return web_ui_icon("models");
    if (has("deployment") || has("run") || has("job") || has("pipeline")) return web_ui_icon("pipeline");
    if (has("trace") || has("span") || has("crash") || has("metric") || has("observ"))
        return web_ui_icon("observability");
    if (has("user") || has("role") || has("polic") || has("token") || has("consent") || has("audit") ||
        has("govern"))
        return web_ui_icon("governance");
    return web_ui_icon("overview");
}

std::string web_ui_panel(std::string_view title, std::string_view sub, std::string_view body) {
    std::string out = "<section class=\"panel\"><div class=\"phead\"><span class=\"ico\">" +
                      panel_icon_for(title) + "</span><h2>" + web_ui_escape(title) + "</h2>";
    if (!sub.empty()) out += "<span class=\"psub\">" + web_ui_escape(sub) + "</span>";
    out += "</div>";
    out += body;
    out += "</section>";
    return out;
}

std::string web_ui_table(const nlohmann::json& columns, const nlohmann::json& rows) {
    if (!rows.is_array() || rows.empty())
        return "<div class=\"emptybox\"><span class=\"eico\">" + web_ui_icon("inbox") +
               "</span><span class=\"et\">Nothing here yet</span></div>";
    std::vector<std::string> cols;
    if (columns.is_array() && !columns.empty()) {
        for (const auto& c : columns)
            if (c.is_string()) cols.push_back(c.get<std::string>());
    } else if (rows[0].is_object()) {
        for (auto it = rows[0].begin(); it != rows[0].end(); ++it) cols.push_back(it.key());
    }
    // Right-align a column whose first present value is numeric (tabular numbers read better right-aligned).
    std::vector<bool> numeric(cols.size(), false);
    for (std::size_t i = 0; i < cols.size(); ++i)
        for (const auto& row : rows)
            if (row.is_object() && row.contains(cols[i]) && !row.at(cols[i]).is_null()) {
                numeric[i] = row.at(cols[i]).is_number();
                break;
            }
    std::string out = "<div class=\"twrap\"><table><thead><tr>";
    for (std::size_t i = 0; i < cols.size(); ++i) {
        std::string label = cols[i];
        std::replace(label.begin(), label.end(), '_', ' ');
        out += std::string("<th") + (numeric[i] ? " class=\"num\"" : "") + ">" + web_ui_escape(label) +
               "</th>";
    }
    out += "</tr></thead><tbody>";
    for (const auto& row : rows) {
        out += "<tr>";
        for (std::size_t i = 0; i < cols.size(); ++i) {
            const nlohmann::json& cell =
                (row.is_object() && row.contains(cols[i])) ? row.at(cols[i]) : nlohmann::json();
            out += std::string("<td") + (numeric[i] ? " class=\"num\"" : "") + ">" +
                   fmt_cell(cols[i], cell) + "</td>";
        }
        out += "</tr>";
    }
    out += "</tbody></table></div>";
    return out;
}

std::string web_ui_overview_cards(const nlohmann::json& info) {
    auto count = [&](const char* key) -> std::string {
        if (info.contains(key) && info[key].is_number()) return fmt_int(info[key].get<long long>());
        return "&middot;";
    };
    long long nodes = (info.contains("cluster_members") && info["cluster_members"].is_array())
                          ? static_cast<long long>(info["cluster_members"].size())
                          : 0;
    struct Card { const char* label; const char* icon; std::string value; };
    const std::array<Card, 6> cards = {{{"Nodes", "nodes", fmt_int(nodes)},
                                        {"Databases", "data", count("databases")},
                                        {"Projects", "projects", count("projects")},
                                        {"Models", "models", count("models")},
                                        {"Runs", "runs", count("runs")},
                                        {"Jobs", "jobs", count("jobs")}}};
    std::string out = "<div class=\"cards\">";
    for (const auto& c : cards)
        out += "<div class=\"card\"><div class=\"chd\"><span class=\"cico\">" + web_ui_icon(c.icon) +
               "</span></div><div class=\"num\">" + c.value + "</div><div class=\"lbl\">" +
               c.label + "</div></div>";
    out += "</div>";
    return out;
}

std::string web_ui_stylesheet() { return std::string(kCss); }

std::string web_ui_favicon() {
    return R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><rect width="32" height="32" rx="7" fill="#2563EB"/><text x="16" y="23" font-family="Geist,-apple-system,Helvetica,Arial,sans-serif" font-size="20" font-weight="600" fill="#fff" text-anchor="middle">O</text></svg>)SVG";
}

namespace {
// Shared client helpers for the interactive tools (Shell + Notebooks): POST a command to the
// cookie-authed /shell endpoint and format a SQL result as a monospace text table.
constexpr std::string_view kSharedShellJs = R"JS(
function zEsc(s){return String(s==null?'':s).replace(/[&<>]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
async function zExec(q){var t0=performance.now();var res=await fetch('/shell',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'q='+encodeURIComponent(q)});var j=await res.json();if(j&&typeof j==='object')j.__ms=Math.round(performance.now()-t0);return j;}
// The formatted execution time, shown bottom-right after each run: the engine's own
// elapsed when it reports it (Python), else the measured round-trip.
function zTime(r){
  if(!r) return '';
  var lbl=r.elapsed?r.elapsed:(typeof r.__ms==='number'?(r.__ms<1000?r.__ms+' ms':(r.__ms/1000).toFixed(2)+' s'):'');
  return lbl?'<div class="exectime">'+zEsc(lbl)+'</div>':'';
}
// When column order isn't given by the server (SELECT * yields keys in map order),
// lead with the identifier-ish columns (name/table/id/…) so the row's subject reads first.
function zOrder(keys){
  var lead=keys.filter(function(c){return /^(name|table|dataset|id|key|project|run|model|node|user|role)$/i.test(c)||/_id$|_name$/i.test(c);});
  var rest=keys.filter(function(c){return lead.indexOf(c)<0;});
  return lead.concat(rest);
}
function zFmt(r){
  if(!r||r.error) return '<pre class="txt err">error: '+zEsc((r&&r.error)?r.error:'request failed')+'</pre>';
  var rows=r.rows||[];
  var arr=rows.length>0&&Array.isArray(rows[0]);           // Python (repl::Result) rows are positional arrays; SQL rows are objects
  var cols=(r.columns&&r.columns.length)?r.columns
           :(!arr&&rows.length&&rows[0]&&typeof rows[0]==='object'?zOrder(Object.keys(rows[0])):[]);
  var msgs=r.messages||[];
  var note=msgs.length?'<div class="rcount">'+zEsc(msgs.join(' · '))+'</div>':'';   // · = middle dot; \xC2\xB7 in a raw string ships literally and JS decodes it to "Â·"
  if(!cols.length){
    if(rows.length) return '<pre class="txt">'+zEsc(JSON.stringify(rows,null,2))+'</pre>'+note;
    if(msgs.length) return '<pre class="txt">'+zEsc(msgs.join('\n'))+'</pre>';   // /help + status render as proper lines, not one ·-crammed row
    return '<pre class="txt">'+zEsc(JSON.stringify(r.value!==undefined?r.value:r,null,2))+'</pre>';
  }
  if(!rows.length) return note||'<div class="rcount">(0 rows)</div>';
  var h='<table class="rtab"><thead><tr>';
  cols.forEach(function(c){h+='<th>'+zEsc(c)+'</th>';});
  h+='</tr></thead><tbody>';
  rows.forEach(function(row){h+='<tr>';cols.forEach(function(c,ci){var v=arr?row[ci]:row[c];v=(v==null?'·':(typeof v==='object'?JSON.stringify(v):String(v)));h+='<td>'+zEsc(v)+'</td>';});h+='</tr>';});
  return h+'</tbody></table>'+note+'<div class="rcount">('+rows.length+' row'+(rows.length===1?'':'s')+')</div>';
}
)JS";
}  // namespace

std::string web_ui_shell_repl() {
    std::string out =
        "<section class=\"panel\"><div class=\"phead\"><span class=\"ico\">" + web_ui_icon("shell") +
        "</span><h2>Shell</h2>"
        "<span class=\"psub\">SQL or Python, multi-line \xC2\xB7 Enter = newline, Shift/\xE2\x8C\x98+Enter runs \xC2\xB7 type / for commands</span></div>"
        "<div class=\"term\" id=\"term\"><div class=\"q\">Orama Zetta console \xC2\xB7 type SQL or Python \xC2\xB7 <b>/</b> for commands.</div></div>"
        "<div class=\"shellrow\"><textarea id=\"cmd\" rows=\"1\" placeholder=\"SELECT * FROM system.nodes   \xE2\x80\x94 or type /\" autocomplete=\"off\" spellcheck=\"false\" autofocus></textarea>"
        "<button class=\"btn btn-primary\" id=\"run\">Run</button>"
        "<div class=\"acmenu\" id=\"acmenu\"></div></div></section><script>";
    out += kSharedShellJs;
    out += R"JS(
(function(){
  var term=document.getElementById('term'),cmd=document.getElementById('cmd'),run=document.getElementById('run'),
      menu=document.getElementById('acmenu'),hist=[],hi=-1;
  // Slash-command palette — typing "/" opens a filtered, scrollable, keyboard-navigable list.
  // The server does the real routing; this is discovery. ↑↓ navigate · Tab fills · Enter runs.
  var CMDS=[
    {c:'/help',d:'show commands'},
    {c:'/sql ',d:'force SQL'},
    {c:'/py ',d:'force Python · compiled, gated, audited'},
    {c:'/tables',d:'list the system.* tables'},
    {c:'/clear',d:'clear the transcript'}
  ];
  var acRows=[],acSel=0;
  function acOpen(){return menu.classList.contains('show');}
  function acClose(){menu.classList.remove('show');}
  function grow(){cmd.style.height='auto';cmd.style.height=Math.min(cmd.scrollHeight,240)+'px';}
  function acFilter(){
    // Only offer the palette for a single-line /command (not inside a multi-line program).
    if(cmd.value.charAt(0)!=='/'||cmd.value.indexOf('\n')>=0){acClose();return;}
    var q=cmd.value.trim().toLowerCase();
    acRows=CMDS.filter(function(x){return q==='/'||x.c.indexOf(q)===0;});
    if(!acRows.length){acClose();return;}
    if(acSel>=acRows.length)acSel=acRows.length-1; if(acSel<0)acSel=0;
    menu.innerHTML=acRows.map(function(x,i){return '<div class="acitem'+(i===acSel?' sel':'')+'" data-i="'+i+'"><span class="acname">'+zEsc(x.c.trim())+'</span><span class="acdesc">'+zEsc(x.d)+'</span></div>';}).join('');
    menu.classList.add('show');
  }
  menu.addEventListener('mousedown',function(e){var it=e.target.closest('.acitem');if(!it)return;e.preventDefault();var x=acRows[+it.getAttribute('data-i')];cmd.value=x.c;acClose();grow();if(x.c.slice(-1)===' '){cmd.focus();var n=cmd.value.length;cmd.setSelectionRange(n,n);}else{go();}});
  cmd.addEventListener('input',function(){acSel=0;grow();acFilter();});
  cmd.addEventListener('blur',function(){setTimeout(acClose,140);});
  async function go(){
    var c=cmd.value.trim(); if(!c) return;
    acClose(); hist.push(c); hi=hist.length; cmd.value=''; grow();
    var lc=c.toLowerCase();
    if(lc==='/clear'||lc==='clear'){ term.innerHTML=''; return; }
    var b=document.createElement('div');
    b.innerHTML='<div><span class="prompt">zetta&gt;</span> <span class="cmd"></span></div><div class="out"></div>';
    b.querySelector('.cmd').textContent=c;   // textContent keeps newlines + escapes safely
    term.appendChild(b); var o=b.querySelector('.out');
    o.innerHTML='<span class="txt">running…</span>'; term.scrollTop=term.scrollHeight;
    try{ var res=await zExec(c); o.innerHTML=zFmt(res)+zTime(res); }catch(e){ o.innerHTML='<pre class="txt err">error: '+zEsc(''+e)+'</pre>'; }
    term.scrollTop=term.scrollHeight;
  }
  run.onclick=go;
  cmd.addEventListener('keydown',function(e){
    if(acOpen()&&acRows.length){
      if(e.key==='ArrowDown'){acSel=(acSel+1)%acRows.length;acFilter();e.preventDefault();return;}
      if(e.key==='ArrowUp'){acSel=(acSel-1+acRows.length)%acRows.length;acFilter();e.preventDefault();return;}
      if(e.key==='Escape'){acClose();e.preventDefault();return;}
      if(e.key==='Tab'){var x=acRows[acSel];cmd.value=x.c;acClose();grow();var n=cmd.value.length;cmd.setSelectionRange(n,n);e.preventDefault();return;}
      if(e.key==='Enter'){var s=acRows[acSel];cmd.value=s.c;acClose();grow();e.preventDefault();
        if(s.c.slice(-1)===' '){var n=cmd.value.length;cmd.setSelectionRange(n,n);}else{go();} return;}
    }
    // Shift / Cmd / Ctrl + Enter runs; plain Enter inserts a newline (multi-line editing).
    if(e.key==='Enter'&&(e.shiftKey||e.metaKey||e.ctrlKey)){e.preventDefault();go();return;}
    // History recall only at the text boundaries so it never hijacks multi-line cursor moves.
    if(e.key==='ArrowUp'&&cmd.selectionStart===0&&cmd.selectionEnd===0){
      if(hi>0){hi--;cmd.value=hist[hi];grow();}e.preventDefault();return;}
    if(e.key==='ArrowDown'&&cmd.selectionStart===cmd.value.length&&cmd.selectionEnd===cmd.value.length){
      if(hi<hist.length-1){hi++;cmd.value=hist[hi];}else{hi=hist.length;cmd.value='';}grow();e.preventDefault();return;}
  });
  grow();
})();
)JS";
    out += "</script>";
    return out;
}

std::string web_ui_notebooks() {
    std::string out =
        "<section class=\"panel\"><div class=\"phead\"><span class=\"ico\">" + web_ui_icon("notebooks") +
        "</span><h2>Notebooks</h2>"
        "<span class=\"psub\">SQL or Python cells (auto-detected, /commands too) \xC2\xB7 Shift/Cmd/Ctrl+Enter runs a cell \xC2\xB7 saved in this browser</span></div>"
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
  async function runCell(i,d){var o=d.querySelector('.out');o.innerHTML='<span class="txt">running…</span>';try{var res=await zExec(book[i].src);book[i].out=zFmt(res)+zTime(res);o.innerHTML=book[i].out;}catch(e){o.innerHTML='<pre class="txt err">error: '+zEsc(''+e)+'</pre>';book[i].out='';}}
  function render(){
    cellsEl.innerHTML='';
    book.forEach(function(cell,i){
      var d=document.createElement('div');d.className='cell';
      d.innerHTML='<div class="cellbar"><span class="q mono">['+(i+1)+']</span><button class="btn small run">Run</button><button class="btn small del">✕</button></div><textarea class="src" spellcheck="false"></textarea><div class="out">'+(cell.out||'')+'</div>';
      var ta=d.querySelector('.src');ta.value=cell.src;
      ta.addEventListener('input',function(){cell.src=this.value;});
      ta.addEventListener('keydown',function(e){if((e.metaKey||e.ctrlKey||e.shiftKey)&&e.key==='Enter'){e.preventDefault();runCell(i,d);}});
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
