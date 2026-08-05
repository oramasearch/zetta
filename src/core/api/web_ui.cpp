#include "zetta/api/web_ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

namespace zetta::api {

namespace {

// The whole design lives in one inline stylesheet (no external assets — air-gapped nodes have no
// egress). Orama Studio design system (imported from oramasearch/orama-design, #874): neutral
// operational surfaces, ink text, ONE action hue — Carbon Blue 60 — used surgically (the brand dot /
// active icon / focus ring / primary action; red is reserved for alarms only), semantic
// green/amber/red status always paired with text, IBM Plex Sans / IBM Plex Mono with a system-font
// fallback (font files are a documented follow-up — no external font requests, ever). Paired light +
// dark themes: every color is a custom property; dark overrides ride prefers-color-scheme. Mobile-
// first: the top bar wraps, the nav scrolls horizontally, content is one full-width column, tables
// scroll.
constexpr std::string_view kCss = R"CSS(
@font-face{font-family:'IBM Plex Sans';font-style:normal;font-weight:400;font-display:swap;src:url('/fonts/plex-sans-400.woff2') format('woff2');}
@font-face{font-family:'IBM Plex Sans';font-style:normal;font-weight:500;font-display:swap;src:url('/fonts/plex-sans-500.woff2') format('woff2');}
@font-face{font-family:'IBM Plex Sans';font-style:normal;font-weight:600;font-display:swap;src:url('/fonts/plex-sans-600.woff2') format('woff2');}
@font-face{font-family:'IBM Plex Mono';font-style:normal;font-weight:400;font-display:swap;src:url('/fonts/plex-mono-400.woff2') format('woff2');}
:root{
  color-scheme:light dark;
  --accent:#0F62FE; --accent-wash:rgba(15,98,254,.08); --accent-hover:#0043CE;
  --accent-solid:#0F62FE; --accent-solid-hover:#0353E9;
  --ink-primary:#161616; --ink-secondary:#393939; --ink-muted:#525252; --ink-quiet:#6F6F6F;
  --surface:#FFFFFF; --canvas:#F4F4F4; --sidebar:#F0F0F0; --border:#E0E0E0;
  --rule-hair:rgba(0,0,0,.07); --rule-card:rgba(0,0,0,.09); --rule-emph:rgba(0,0,0,.14);
  --wash-hover:rgba(0,0,0,.04); --wash-selected:rgba(0,0,0,.06);
  --ok-fg:#0E6027; --ok-bg:#DEFBE6; --warn-fg:#684E00; --warn-bg:#FCF4D6; --err-fg:#A2191F; --err-bg:#FFF1F1;
  --info-fg:#0043CE; --info-bg:#EDF5FF;
  --dot-active:#24A148; --dot-inactive:#8D8D8D;
  --shadow-sm:0 1px 2px rgba(22,22,22,.04),0 1px 1px rgba(22,22,22,.03);
  --shadow-md:0 6px 22px rgba(22,22,22,.06),0 2px 6px rgba(22,22,22,.04);
  --shadow-pop:0 12px 36px rgba(22,22,22,.14);
  --font-sans:'IBM Plex Sans',-apple-system,BlinkMacSystemFont,'Segoe UI','Helvetica Neue',Arial,sans-serif;
  --font-mono:'IBM Plex Mono',ui-monospace,SFMono-Regular,'SF Mono',Menlo,Consolas,monospace;
}
/* The paired dark theme: the same overrides ride the OS preference (unless the
   user pinned light) AND the explicit topbar toggle (data-theme on <html>). */
:root[data-theme=dark]{
  --accent:#78A9FF; --accent-wash:rgba(120,169,255,.14); --accent-hover:#A6C8FF;
  --accent-solid:#0F62FE; --accent-solid-hover:#0353E9;
  --ink-primary:#F4F4F4; --ink-secondary:#C6C6C6; --ink-muted:#A8A8A8; --ink-quiet:#8D8D8D;
  --surface:#262626; --canvas:#161616; --sidebar:#212121; --border:#393939;
  --rule-hair:rgba(255,255,255,.08); --rule-card:rgba(255,255,255,.10); --rule-emph:rgba(255,255,255,.16);
  --wash-hover:rgba(255,255,255,.05); --wash-selected:rgba(255,255,255,.08);
  --ok-fg:#42BE65; --ok-bg:rgba(66,190,101,.14); --warn-fg:#F1C21B; --warn-bg:rgba(241,194,27,.12);
  --err-fg:#FA4D56; --err-bg:rgba(250,77,86,.12);
  --info-fg:#78A9FF; --info-bg:rgba(120,169,255,.12);
  --dot-active:#42BE65; --dot-inactive:#6F6F6F;
  --shadow-sm:0 1px 2px rgba(0,0,0,.5),0 1px 1px rgba(0,0,0,.4);
  --shadow-md:0 6px 22px rgba(0,0,0,.5),0 2px 6px rgba(0,0,0,.35);
  --shadow-pop:0 12px 36px rgba(0,0,0,.6);
}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){
  --accent:#78A9FF; --accent-wash:rgba(120,169,255,.14); --accent-hover:#A6C8FF;
  --accent-solid:#0F62FE; --accent-solid-hover:#0353E9;
  --ink-primary:#F4F4F4; --ink-secondary:#C6C6C6; --ink-muted:#A8A8A8; --ink-quiet:#8D8D8D;
  --surface:#262626; --canvas:#161616; --sidebar:#212121; --border:#393939;
  --rule-hair:rgba(255,255,255,.08); --rule-card:rgba(255,255,255,.10); --rule-emph:rgba(255,255,255,.16);
  --wash-hover:rgba(255,255,255,.05); --wash-selected:rgba(255,255,255,.08);
  --ok-fg:#42BE65; --ok-bg:rgba(66,190,101,.14); --warn-fg:#F1C21B; --warn-bg:rgba(241,194,27,.12);
  --err-fg:#FA4D56; --err-bg:rgba(250,77,86,.12);
  --info-fg:#78A9FF; --info-bg:rgba(120,169,255,.12);
  --dot-active:#42BE65; --dot-inactive:#6F6F6F;
  --shadow-sm:0 1px 2px rgba(0,0,0,.5),0 1px 1px rgba(0,0,0,.4);
  --shadow-md:0 6px 22px rgba(0,0,0,.5),0 2px 6px rgba(0,0,0,.35);
  --shadow-pop:0 12px 36px rgba(0,0,0,.6);
}}
*{box-sizing:border-box;}
html,body{margin:0;}
body{background:var(--canvas);color:var(--ink-primary);font:14px/1.55 var(--font-sans);
  -webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale;}
.mono,code,pre{font-family:var(--font-mono);font-variant-numeric:tabular-nums;}
.q{color:var(--ink-quiet);}
a{color:var(--ink-secondary);text-decoration:none;}

.top{display:flex;align-items:center;flex-wrap:wrap;gap:8px 14px;padding:10px 16px;background:var(--surface);
  border-bottom:1px solid var(--rule-hair);position:sticky;top:0;z-index:10;}
.brand{display:flex;align-items:center;gap:8px;font-size:16px;font-weight:500;letter-spacing:-.01em;color:var(--ink-primary);}
.brand .live{width:8px;height:8px;border-radius:50%;background:var(--accent);flex:none;}
.brand.big{font-size:21px;}
.spacer{flex:1;min-width:8px;}
.meta{font:11px/1.3 var(--font-mono);color:var(--ink-quiet);}
.user{font-size:12px;color:var(--ink-muted);}
.util{width:34px;height:34px;border-radius:9px;display:inline-flex;align-items:center;justify-content:center;color:var(--ink-muted);transition:background .12s,color .12s;}
.util:hover{background:var(--wash-hover);color:var(--ink-primary);}
.util svg{width:17px;height:17px;}
.connpill{display:inline-flex;align-items:center;font:11px/1 var(--font-mono);font-weight:500;text-transform:uppercase;
  letter-spacing:.04em;padding:4px 8px;border-radius:8px;background:var(--wash-hover);color:var(--ink-quiet);}
.connpill.ok{color:var(--ok-fg);background:var(--ok-bg);}
form.inline{display:inline;margin:0;}

.layout{display:grid;grid-template-columns:224px 1fr;align-items:start;}
.side{background:var(--sidebar);border-right:1px solid var(--rule-hair);padding:14px 10px;
  position:sticky;top:52px;align-self:stretch;min-height:calc(100vh - 52px);
  display:flex;flex-direction:column;max-height:calc(100vh - 52px);overflow-y:auto;}
.side .badge{margin-left:auto;font:10.5px/1 var(--font-mono);color:var(--ink-quiet);
  background:var(--wash-hover);border-radius:12px;padding:3px 8px;flex:none;}
.sideuser{margin-top:auto;display:flex;align-items:center;gap:10px;padding:12px 10px 4px;
  border-top:1px solid var(--rule-hair);}
.sideuser .avatar{width:30px;height:30px;border-radius:8px;background:var(--accent-solid);color:#fff;
  display:inline-flex;align-items:center;justify-content:center;font:12px var(--font-mono);
  font-weight:600;text-transform:uppercase;flex:none;}
.sideuser .who{flex:1;min-width:0;}
.sideuser .who b{display:block;font-size:12.5px;font-weight:600;color:var(--ink-primary);
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.sideuser .who span{display:block;font:10.5px/1.4 var(--font-mono);color:var(--ink-quiet);
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.sideuser .btn.small{padding:5px 9px;font-size:11px;flex:none;}

/* The Ask Orama rail (#874): the design's collaborator column — blue header,
   context note, transcript, and the ask box pinned to the bottom. Renders as
   a third grid column on wide screens; below that it collapses away (the Chat
   view stays one click away). */
.rail{background:var(--surface);border-left:1px solid var(--rule-hair);position:sticky;top:52px;
  align-self:stretch;height:calc(100vh - 52px);display:flex;flex-direction:column;padding:14px;gap:12px;}
.railhead{background:var(--accent-solid);color:#fff;border-radius:11px;padding:13px 15px;
  display:flex;flex-direction:column;gap:3px;}
.railhead .rl{font:10.5px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.08em;opacity:.85;}
.railhead b{font-size:15px;font-weight:600;}
.railnote{border:1px solid var(--accent);color:var(--ink-secondary);border-radius:10px;
  padding:10px 12px;font-size:12px;line-height:1.5;}
.railbubble{background:var(--accent-wash);color:var(--ink-secondary);border-radius:10px;
  padding:10px 12px;font-size:12.5px;line-height:1.5;}
.railchat{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:8px;min-height:40px;}
.railchat .rmsg{border-radius:10px;padding:9px 11px;font-size:12.5px;line-height:1.55;max-width:94%;
  white-space:pre-wrap;word-break:break-word;}
.railchat .rme{align-self:flex-end;background:var(--accent-wash);color:var(--ink-primary);}
.railchat .ragent{align-self:flex-start;background:var(--wash-hover);color:var(--ink-secondary);}
.railchat .rthinking{color:var(--ink-quiet);font:11.5px var(--font-mono);}
.railask label{margin:0 0 6px;font:10.5px/1 var(--font-mono);text-transform:uppercase;
  letter-spacing:.08em;color:var(--ink-quiet);}
.railask .rrow{display:flex;gap:8px;align-items:flex-end;}
.railask textarea{min-height:44px;font-family:var(--font-sans);font-size:13px;resize:none;}
.railask .btn-primary{padding:10px 12px;}
.railempty{color:var(--ink-quiet);font-size:12.5px;line-height:1.6;}
.railempty a{color:var(--accent);}
/* The rail is a toggleable drawer (July 24): closed by default, opened from
   the topbar toggle; the choice persists locally (pre-paint pin on <html>). */
.rail{display:none;}
html[data-rail=open] .rail{display:flex;}
@media (min-width:1280px){html[data-rail=open] .layout.withrail{grid-template-columns:224px 1fr 340px;}}
@media (max-width:1279px){html[data-rail=open] .rail{display:none;}}
.util.active{background:var(--accent-wash);color:var(--accent);}

/* Status banners (July 24): live execution, needs-approval, unreachable. */
.bannerrow{display:flex;align-items:center;gap:14px;border-radius:11px;padding:13px 16px;margin-bottom:16px;font-size:13px;}
.bannerrow .bico{width:30px;height:30px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;flex:none;}
.bannerrow .bico svg{width:16px;height:16px;}
.bannerrow .bbody{flex:1;min-width:0;}
.bannerrow .bbody b{display:block;font-size:13.5px;color:var(--ink-primary);}
.bannerrow .bbody span{font-size:12px;color:var(--ink-muted);}
.bannerrow.live{background:var(--surface);border:1px solid var(--rule-card);}
.bannerrow.live .bico{background:var(--accent-solid);color:#fff;}
.bannerrow.warnb{background:var(--warn-bg);border:1px solid rgba(0,0,0,.05);}
.bannerrow.warnb .bico{background:transparent;color:var(--warn-fg);}
.bannerrow.errb{background:var(--err-bg);border:1px solid rgba(0,0,0,.05);}
.bannerrow.errb .bico{background:transparent;color:var(--err-fg);}
.bannerrow .bact{flex:none;}

/* The stat-card row (Infrastructure / Data): big mono number, corner icon,
   footer line + status pill. */
.statgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:14px;margin-bottom:16px;}
.statcard{background:var(--surface);border:1px solid var(--rule-card);border-radius:12px;padding:16px 18px;}
.statcard .schd{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px;}
.statcard .schd span{font-size:12px;color:var(--ink-muted);}
.statcard .scico{width:30px;height:30px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;background:var(--accent-solid);color:#fff;}
.statcard .scico svg{width:16px;height:16px;}
.statcard .num{font:30px/1.1 var(--font-mono);font-weight:500;color:var(--ink-primary);letter-spacing:-.02em;}
.statcard .scft{display:flex;align-items:center;justify-content:space-between;margin-top:8px;font-size:11.5px;color:var(--ink-quiet);}

/* Topbar page title + Connected pill (July 24). */
.pagetitle{display:flex;align-items:center;gap:10px;font-size:14px;font-weight:600;color:var(--ink-primary);
  border-left:2px solid var(--accent);padding-left:12px;}

/* Search + filter segments (Projects, July 24) and segmented page tabs
   (Governance). */
.filterrow{display:flex;gap:12px;align-items:center;margin-bottom:14px;}
.filterrow input[type=search]{flex:1;max-width:340px;}
.filterseg{display:inline-flex;background:var(--wash-hover);border-radius:9px;padding:3px;gap:2px;}
.filterseg a{padding:6px 12px;border-radius:7px;font-size:12px;color:var(--ink-muted);white-space:nowrap;}
.filterseg a:hover{color:var(--ink-primary);}
.filterseg a.on{background:var(--accent-solid);color:#fff;}
.fcount{margin-left:auto;font-size:11.5px;color:var(--ink-quiet);}
.segtabs{display:flex;gap:4px;border-bottom:1px solid var(--rule-hair);margin-bottom:16px;}
.segtabs a{padding:9px 14px;font-size:13px;color:var(--ink-muted);border-bottom:2px solid transparent;margin-bottom:-1px;}
.segtabs a:hover{color:var(--ink-primary);}
.segtabs a.on{color:var(--ink-primary);font-weight:600;border-bottom-color:var(--accent);}
.side .group{font:10.5px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.09em;color:var(--ink-quiet);
  padding:16px 10px 7px;}
.side a{display:flex;align-items:center;gap:10px;padding:8px 10px;margin-bottom:2px;border-radius:9px;color:var(--ink-secondary);
  font-size:13.5px;white-space:nowrap;transition:background .12s,color .12s;}
.side a svg{width:16px;height:16px;flex:none;opacity:.72;}
.side a:hover{background:var(--wash-hover);color:var(--ink-primary);}
.side a.active{background:var(--wash-selected);color:var(--ink-primary);font-weight:500;}
.side a.active svg{opacity:1;color:var(--accent);}
.main{padding:24px 32px 60px;min-width:0;width:100%;max-width:1360px;margin:0 auto;}
/* Wide data surfaces still scroll inside their own containers (.twrap etc.). */

/* Web shell — the console surface; rides the theme like everything else */
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
.exectime{text-align:right;color:var(--ink-quiet);font:11px/1 var(--font-mono);margin-top:8px;padding-right:8px;}
.exectime:empty{display:none;}
.term .cmd{color:var(--ink-primary);white-space:pre-wrap;}
.shellrow{display:flex;gap:8px;margin-top:14px;position:relative;align-items:flex-end;}
.shellrow textarea{font:14.5px/1.6 var(--font-mono);padding:12px 14px;min-height:48px;max-height:240px;resize:none;overflow-y:auto;}
.acmenu{position:absolute;top:calc(100% + 6px);left:0;right:0;background:var(--surface);border:1px solid var(--rule-emph);
  border-radius:11px;box-shadow:var(--shadow-pop);max-height:238px;overflow-y:auto;z-index:40;display:none;padding:6px;}
.acmenu.show{display:block;}
.acitem{display:flex;justify-content:space-between;align-items:baseline;gap:18px;padding:9px 11px;border-radius:8px;cursor:pointer;}
.acitem .acname{font:13.5px var(--font-mono);color:var(--ink-primary);white-space:nowrap;}
.acitem .acdesc{font:12px var(--font-sans);color:var(--ink-quiet);text-align:right;}
.acitem.sel,.acitem:hover{background:var(--accent-wash);}
.acitem.sel .acname{color:var(--accent);}

/* Pagination + capability action forms + notebooks */
.pager{display:flex;gap:12px;align-items:center;margin-top:12px;font:11.5px/1 var(--font-mono);color:var(--ink-quiet);}
.pager a{color:var(--ink-secondary);padding:5px 11px;border:1px solid var(--rule-emph);border-radius:7px;background:var(--surface);}
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
  .side{position:sticky;top:52px;min-height:0;max-height:none;display:flex;flex-direction:row;gap:2px;overflow-x:auto;
    -webkit-overflow-scrolling:touch;border-right:none;border-bottom:1px solid var(--rule-hair);padding:6px 10px;z-index:9;}
  .side .group,.side .badge,.sideuser{display:none;}
  .main{padding:14px;}
  body{font-size:13.5px;}
  .card .num{font-size:23px;}
  .phead h2{font-size:14px;}
  table{font-size:12px;} th,td{padding:8px 10px;}
  .term{font-size:13px;padding:15px 16px;} .shellrow textarea{font-size:13px;}
}
/* The Studio page hero (every view, July 24 design): mono eyebrow, huge
   title, one-line sub, and the mono meta strip with hairline separators.
   Optional actions cluster floats top-right. */
.hero{margin:26px 2px 30px;display:flex;gap:24px;align-items:flex-start;}
.hero .hbody{flex:1;min-width:0;}
.hero .heyebrow{font:11px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.12em;color:var(--ink-quiet);margin-bottom:14px;}
.hero h1{margin:0 0 10px;font-size:34px;line-height:1.15;font-weight:600;letter-spacing:-.02em;color:var(--ink-primary);}
.hero .hsub{font-size:14px;color:var(--ink-muted);margin-bottom:16px;max-width:640px;line-height:1.6;}
.hero .hmeta{display:flex;flex-wrap:wrap;gap:8px 0;font-size:12px;color:var(--ink-quiet);align-items:center;}
.hero .hmeta span{display:inline-flex;align-items:center;gap:7px;}
.hero .hmeta span+span::before{content:'';width:1px;height:14px;background:var(--rule-emph);margin:0 16px;}
.hero .hmeta b{font:12px var(--font-mono);color:var(--ink-primary);font-weight:500;}
.hero .hactions{display:flex;gap:9px;flex:none;padding-top:34px;}
.paths{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-bottom:16px;}
@media (max-width:900px){.paths{grid-template-columns:1fr;} .hero h1{font-size:26px;}}
.path{display:flex;flex-direction:column;background:var(--surface);border:1px solid var(--rule-card);border-radius:12px;padding:20px 22px;}
.path .peyebrow{display:flex;align-items:center;gap:12px;font:11px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.08em;color:var(--accent);margin-bottom:12px;}
.path .pico{width:40px;height:40px;border-radius:10px;display:inline-flex;align-items:center;justify-content:center;background:var(--accent-wash);color:var(--accent);flex:none;}
.path .pico svg{width:19px;height:19px;}
.path h3{margin:0 0 8px;font-size:19px;font-weight:600;letter-spacing:-.015em;color:var(--ink-primary);}
.path .pbody{font-size:13.5px;line-height:1.6;color:var(--ink-muted);flex:1;}
.path .pnote{display:inline-flex;align-items:center;gap:7px;font-size:12px;color:var(--ink-quiet);background:var(--wash-hover);border-radius:8px;padding:6px 10px;margin-top:12px;}
.path .pnote .dot{flex:none;}
.path .pact{margin-top:16px;padding-top:14px;border-top:1px solid var(--rule-hair);
  display:flex;justify-content:flex-end;}
.path .pact a{display:inline-flex;align-items:center;gap:7px;font-size:13.5px;font-weight:500;color:var(--accent);}
.path .pact a svg{width:15px;height:15px;}
.path.muted .pico{background:var(--wash-hover);color:var(--ink-quiet);}
.path.muted .peyebrow{color:var(--ink-quiet);}
.worklist{display:flex;flex-direction:column;}
.workrow{display:flex;align-items:center;gap:16px;padding:13px 4px;border-bottom:1px solid var(--rule-hair);color:inherit;}
.workrow:last-child{border-bottom:none;}
.workrow:hover{background:var(--wash-hover);}
.workrow .wstat{flex:0 0 118px;display:flex;flex-direction:column;gap:4px;align-items:flex-start;}
.workrow .wage{font:11px var(--font-mono);color:var(--ink-quiet);}
.workrow .wname{flex:1;font-size:14px;font-weight:600;color:var(--ink-primary);min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.workrow .wnext{font-size:13px;color:var(--ink-muted);flex:none;}
.workrow .warr{color:var(--accent);flex:none;display:inline-flex;}
.workrow .warr svg{width:16px;height:16px;}
.panel{background:var(--surface);border:1px solid var(--rule-card);border-radius:12px;padding:18px 20px;margin-bottom:16px;}
/* Section header, July 24 design: the small accent square + bold title + quiet
   sub. The icon tile is retired (kept in markup, hidden here). */
.phead{display:flex;align-items:center;gap:10px;margin-bottom:15px;flex-wrap:wrap;}
.phead::before{content:'';width:8px;height:8px;background:var(--accent);border-radius:2px;flex:none;}
.phead .ico{display:none;}
.phead h2{margin:0;font-size:15px;font-weight:600;letter-spacing:-.01em;color:var(--ink-primary);}
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
.card{display:block;color:inherit;text-decoration:none;background:var(--surface);border:1px solid var(--rule-card);border-radius:12px;padding:16px;
  transition:border-color .14s ease,background .14s ease;}
.card:hover{border-color:var(--rule-emph);background:var(--wash-hover);}
.card:hover .cico{background:var(--accent-solid);color:#fff;}
.card .chd{display:flex;align-items:center;justify-content:space-between;margin-bottom:13px;}
.card .cico{width:32px;height:32px;border-radius:9px;display:inline-flex;align-items:center;justify-content:center;
  background:var(--accent-wash);color:var(--accent);}
.card .cico svg{width:17px;height:17px;stroke-width:2;}
.card .num{font:29px/1 var(--font-mono);font-weight:500;color:var(--ink-primary);letter-spacing:-.02em;font-variant-numeric:tabular-nums;}
.card .lbl{margin-top:8px;font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.05em;color:var(--ink-muted);}
.metaline{margin-top:14px;font-size:11px;color:var(--ink-quiet);}

/* Explanatory legend (e.g. the pipeline model on the Pipeline view) */
.legend{background:var(--accent-wash);border:1px solid var(--rule-card);border-radius:14px;padding:16px 18px;margin-bottom:18px;}
.legendhd{display:flex;align-items:center;gap:10px;margin-bottom:12px;}
.legendhd .ico{width:26px;height:26px;border-radius:8px;display:inline-flex;align-items:center;justify-content:center;background:var(--surface);color:var(--accent);flex:none;}
.legendhd .ico svg{width:15px;height:15px;}
.legendhd b{font-size:14px;color:var(--ink-primary);}
.legendflow{display:flex;flex-wrap:wrap;align-items:center;font:13px/1.9 var(--font-mono);color:var(--ink-secondary);margin-bottom:12px;}
.legendflow em{font-style:normal;font-weight:600;color:var(--accent);}
.legendflow span{color:var(--ink-quiet);margin:0 4px;}
.legenddefs{display:grid;gap:7px;font-size:12.5px;color:var(--ink-muted);line-height:1.55;}
.legenddefs b{color:var(--ink-primary);font-weight:600;margin-right:5px;}

.twrap{overflow-x:auto;-webkit-overflow-scrolling:touch;border:1px solid var(--rule-hair);border-radius:11px;}
table{width:100%;border-collapse:collapse;font-size:13px;}
thead th{position:sticky;top:0;background:var(--info-bg);z-index:1;}
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

/* Files (large objects / blobs) panel */
.filebar{display:flex;align-items:center;gap:10px;margin-bottom:14px;flex-wrap:wrap;}
.filebar input[type=file]{width:auto;flex:1;min-width:200px;}
.filelist{display:flex;flex-direction:column;}
.filerow{display:flex;align-items:center;gap:12px;padding:10px 2px;border-bottom:1px solid var(--rule-hair);}
.filerow:last-child{border-bottom:none;}
.filerow .fname{flex:1;font:13px var(--font-mono);color:var(--ink-primary);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
.filerow .fsize{font:12px var(--font-mono);color:var(--ink-quiet);flex:none;}

/* Projects: index list, detail breadcrumb + metadata grid */
.prjlist{border:1px solid var(--rule-hair);border-radius:11px;overflow:hidden;}
.prjlist .exprow{padding:12px 14px;font-size:13.5px;}
.crumb{margin-bottom:16px;font-size:13px;color:var(--ink-quiet);}
.crumb a{color:var(--ink-secondary);border-bottom:1px dotted var(--rule-emph);}
.crumb b{color:var(--ink-primary);}
.kvs{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px 22px;}
.kv{display:flex;flex-direction:column;gap:3px;min-width:0;}
.kv span{font:10.5px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.05em;color:var(--ink-quiet);}
.kv b{font-size:13px;font-weight:500;color:var(--ink-primary);word-break:break-all;}

/* Search: an index picker + query bar, and a ranked list of matching documents. */
.searchbar{display:flex;gap:9px;align-items:center;}
.searchbar select{width:auto;flex:none;max-width:260px;}
.searchbar input{flex:1;width:auto;}
.searchbar .btn{flex:none;}
.hits{display:flex;flex-direction:column;gap:11px;}
.hit{background:var(--surface);border:1px solid var(--rule-card);border-radius:12px;padding:14px 16px;transition:border-color .14s ease;}
.hit:hover{border-color:var(--rule-emph);}
.hitmeta{display:flex;align-items:baseline;gap:10px;font:11px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.04em;color:var(--ink-quiet);margin-bottom:8px;}
.hitmeta .rank{font-weight:600;color:var(--ink-secondary);}
.fld{display:flex;gap:12px;padding:4px 0;font-size:13.5px;line-height:1.5;}
.fld .lbl{flex:0 0 64px;font:10px/1.6 var(--font-mono);text-transform:uppercase;letter-spacing:.05em;color:var(--ink-muted);}
.fld span:last-child{color:var(--ink-primary);min-width:0;word-break:break-word;}

/* Action toolbar + modal dialogs — real affordances, not a stack of open forms. */
.toolbar{display:flex;gap:9px;flex-wrap:wrap;margin-bottom:16px;}
dialog.dlg{border:none;border-radius:16px;padding:0;box-shadow:0 16px 48px rgba(18,20,26,.24);width:min(520px,92vw);color:var(--ink-primary);background:var(--surface);}
dialog.dlg::backdrop{background:rgba(18,20,26,.42);}
dialog.dlg form{padding:22px 24px 20px;margin:0;}
.dlghd{display:flex;align-items:center;justify-content:space-between;gap:10px;}
.dlghd h3{margin:0;font:17px var(--font-sans);font-weight:600;color:var(--ink-primary);}
.dlgx{background:none;border:none;font-size:22px;line-height:1;color:var(--ink-quiet);cursor:pointer;padding:0 4px;}
.dlgsub{margin:6px 0 16px;font-size:13px;color:var(--ink-muted);}
.dlgact{display:flex;justify-content:flex-end;gap:9px;margin-top:22px;}
dialog.dlg .fgrid{gap:12px 18px;}
dialog.dlg label{display:block;font:11px/1.5 var(--font-mono);text-transform:uppercase;letter-spacing:.04em;color:var(--ink-muted);margin-bottom:5px;}
dialog.dlg select,dialog.dlg input{width:100%;}
/* Per-section header actions (icon + task label buttons). */
.pactions{margin-left:auto;display:flex;gap:8px;flex-wrap:wrap;}
.btn.small{display:inline-flex;align-items:center;gap:6px;}
.btn svg{width:14px;height:14px;flex:none;opacity:.75;}
.btn-primary svg{opacity:1;}

/* Visual graph editor — an interactive execution-graph canvas. */
.getoolbar{display:flex;align-items:center;gap:9px;margin-bottom:14px;}
.getoolbar select{width:auto;max-width:340px;}
.getoolbar .gekind{max-width:132px;color:var(--ink-secondary);}
.gespring{flex:1;}
.gecanvas{position:relative;overflow:auto;border:1px solid var(--rule-card);border-radius:12px;
  background-color:var(--canvas);background-image:radial-gradient(circle,rgba(0,0,0,.06) 1px,transparent 1.3px);
  background-size:22px 22px;min-height:340px;max-height:600px;}
.gesvg{display:block;}
.genode{cursor:grab;}
.genode:active{cursor:grabbing;}
.generect{fill:var(--surface);stroke:var(--rule-emph);stroke-width:1.4;}
.genode:hover .generect{stroke:var(--accent);}
.genkind{font:10px var(--font-mono);text-transform:uppercase;letter-spacing:.06em;fill:var(--ink-quiet);}
.genname{font:13px var(--font-sans);font-weight:500;fill:var(--ink-primary);}
.geport{fill:var(--accent);stroke:var(--surface);stroke-width:2;cursor:crosshair;}
.gedel{font:15px var(--font-sans);fill:var(--ink-quiet);cursor:pointer;opacity:0;text-anchor:middle;}
.genode:hover .gedel{opacity:1;}
.gedel:hover{fill:var(--err-fg);}
.geedge{fill:none;stroke:#9aa1ad;stroke-width:1.8;cursor:pointer;}
.geedge:hover{stroke:var(--err-fg);stroke-width:2.4;}
.geedge.ctrl{stroke-dasharray:5 4;}
.geghost{stroke:var(--accent);stroke-dasharray:4 4;pointer-events:none;}
.gehint{margin-top:10px;font-size:11.5px;color:var(--ink-quiet);}
.genameedit{position:absolute;width:132px;font:13px var(--font-sans);padding:4px 7px;border:1.5px solid var(--accent);border-radius:7px;background:var(--surface);color:var(--ink-primary);z-index:5;box-shadow:var(--shadow-pop);}
.getoast{position:fixed;right:24px;bottom:24px;background:var(--ink-primary);color:var(--surface);font:13px var(--font-sans);font-weight:500;padding:11px 17px;border-radius:11px;box-shadow:var(--shadow-pop);opacity:0;transform:translateY(8px);transition:opacity .2s ease,transform .2s ease;pointer-events:none;z-index:60;}
.getoast.show{opacity:1;transform:translateY(0);}
.getoast.err{background:var(--err-fg);}
.gk-connect .generect{stroke:#0F62FE;} .gk-clean .generect{stroke:#0E6027;}
.gk-model .generect{stroke:#7C3AED;} .gk-index .generect{stroke:#B45309;}
.gk-serve .generect{stroke:#0891B2;} .gk-act .generect{stroke:#BE185D;}

.pill{display:inline-flex;align-items:center;gap:6px;font:10px/1 var(--font-mono);font-weight:500;text-transform:uppercase;
  letter-spacing:.05em;border-radius:999px;padding:4px 9px;color:var(--ink-secondary);background:var(--wash-hover);}
.pill::before{content:'';width:6px;height:6px;border-radius:50%;background:currentColor;flex:none;}
.pill.ok{color:var(--ok-fg);background:var(--ok-bg);} .pill.warn{color:var(--warn-fg);background:var(--warn-bg);}
.pill.err{color:var(--err-fg);background:var(--err-bg);}
.status{display:inline-flex;align-items:center;gap:6px;color:var(--ink-secondary);}
.dot{width:8px;height:8px;border-radius:50%;background:var(--dot-inactive);}
.status.ok .dot{background:var(--dot-active);}

.btn{font:13.5px/1 var(--font-sans);font-weight:500;color:var(--ink-primary);background:var(--surface);border:1px solid var(--rule-emph);
  border-radius:9px;padding:10px 15px;cursor:pointer;transition:background .12s,border-color .12s,box-shadow .12s;}
.btn:hover{background:var(--wash-hover);}
.btn.small{padding:6px 11px;font-size:12px;border-radius:8px;}
.btn-primary{display:inline-flex;align-items:center;gap:7px;background:var(--accent-solid);color:#fff;border-color:var(--accent-solid);}
.btn-primary:hover{background:var(--accent-solid-hover);border-color:var(--accent-solid-hover);}
.btn-primary svg{width:14px;height:14px;}
input,textarea,select{font:14px var(--font-sans);padding:10px 12px;border:1px solid var(--rule-emph);border-radius:9px;
  background:var(--canvas);color:var(--ink-primary);width:100%;transition:border-color .12s,box-shadow .12s,background .12s;}
input:focus,textarea:focus,select:focus{outline:none;border-color:var(--accent);background:var(--surface);box-shadow:0 0 0 3px var(--accent-wash);}
input::placeholder{color:var(--ink-quiet);}
textarea{font-family:var(--font-mono);font-size:13px;min-height:92px;resize:vertical;}
label{display:block;margin:12px 0 5px;font-size:12px;font-weight:500;color:var(--ink-secondary);}
label .req{color:var(--accent);margin-left:2px;}
pre{margin:8px 0 0;padding:13px;white-space:pre-wrap;word-break:break-word;font-size:12px;line-height:1.55;
  color:var(--ink-primary);background:var(--canvas);border:1px solid var(--rule-hair);border-radius:9px;overflow-x:auto;}

/* Collapsible action forms (native details/summary) + 2-col field grid */
.action{border:1px solid var(--rule-card);border-radius:12px;margin-bottom:12px;background:var(--surface);overflow:hidden;transition:box-shadow .14s;}
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
.agent-edit{display:flex;flex-direction:column;gap:6px;padding:10px 0 4px;max-width:560px;}
.agent-edit input,.agent-edit textarea{width:100%;}
.agent-edit .dlgact{margin-top:8px;}
details>summary.btn{display:inline-block;cursor:pointer;list-style:none;}
details>summary.btn::-webkit-details-marker{display:none;}

/* A nested object input (the connector config on create_dataset_from_query,
   #827): its own flat surface inside the dialog's grid, so the structure of
   the input is visible instead of collapsed into one opaque text box. */
.fgroup{border:1px solid var(--rule-hair);border-radius:10px;padding:2px 14px 12px;
  margin:6px 0 2px;background:var(--canvas);}
.fgroup>legend{font-size:11px;font-weight:600;letter-spacing:.04em;text-transform:uppercase;
  color:var(--ink-quiet);padding:0 6px;}
.fgroup input[type=password]{letter-spacing:.12em;}

.loginwrap{min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
.logincard{background:var(--surface);border:1px solid var(--rule-emph);border-radius:14px;padding:26px;
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
        "<meta name=\"color-scheme\" content=\"light dark\">"
        // Pre-paint theme pin: apply the stored choice before first paint so a
        // pinned theme never flashes the other one. System preference needs no
        // attribute (the media query handles it).
        "<script>try{var t=localStorage.getItem('zetta-theme');"
        "if(t==='light'||t==='dark')document.documentElement.dataset.theme=t;"
        "if(localStorage.getItem('zetta-rail')==='open')document.documentElement.dataset.rail='open';"
        "}catch(e){}</script>"
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

// An ISO-8601 UTC timestamp rendered as a compact relative age ("3m ago",
// "2h ago", "5d ago"), with the full timestamp on hover; older than 30 days
// falls back to the calendar date.
std::string fmt_relative_time(const std::string& iso) {
    std::tm parsed{};
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &parsed.tm_year, &parsed.tm_mon,
                    &parsed.tm_mday, &parsed.tm_hour, &parsed.tm_min, &parsed.tm_sec) != 6)
        return "<span class=\"mono\">" + web_ui_escape(iso) + "</span>";
    parsed.tm_year -= 1900;
    parsed.tm_mon -= 1;
    const std::time_t then = timegm(&parsed);
    const std::time_t now = std::time(nullptr);
    const long diff = static_cast<long>(now - then);
    std::string rel;
    if (diff < 0) rel = "just now";
    else if (diff < 60) rel = std::to_string(diff) + "s ago";
    else if (diff < 3600) rel = std::to_string(diff / 60) + "m ago";
    else if (diff < 86400) rel = std::to_string(diff / 3600) + "h ago";
    else if (diff < 30 * 86400) rel = std::to_string(diff / 86400) + "d ago";
    else rel = iso.substr(0, 10);
    return "<span class=\"mono\" title=\"" + web_ui_escape(iso) + "\">" + rel + "</span>";
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
        const std::string full = value.dump();
        std::string s = full;
        if (s.size() > 88) s = s.substr(0, 88) + "\xE2\x80\xA6";
        return "<code class=\"j\" title=\"" + web_ui_escape(full) + "\">" +
               web_ui_escape(s) + "</code>";
    }
    const std::string s = value.get<std::string>();
    if (s.empty()) return "<span class=\"q\">&middot;</span>";
    // Timestamp columns render as a relative age, full value on hover.
    if (contains_any(col, {"created", "updated", "_at", "started", "expires", "time", "fresh",
                           "seen", "last_"}) &&
        s.size() >= 19 && s[4] == '-' && s[10] == 'T')
        return fmt_relative_time(s);
    // Opaque ids: mono, truncated to a readable head with the full id on hover.
    if (contains_any(col, {"_id", "trace", "span", "hash", "sha256"}) || col == "id") {
        if (s.size() > 18)
            return "<span class=\"mono\" title=\"" + web_ui_escape(s) + "\">" +
                   web_ui_escape(s.substr(0, 12)) + "\xE2\x80\xA6</span>";
        return "<span class=\"mono\">" + web_ui_escape(s) + "</span>";
    }
    if (contains_any(col, {"node", "address", "path", "endpoint", "version", "ref", "token",
                           "owner", "columns", "fields", "scope", "permissions", "roles"})) {
        if (s.size() > 64)
            return "<span class=\"mono\" title=\"" + web_ui_escape(s) + "\">" +
                   web_ui_escape(s.substr(0, 64)) + "\xE2\x80\xA6</span>";
        return "<span class=\"mono\">" + web_ui_escape(s) + "</span>";
    }
    // Long free text (e.g. a capabilities list) truncates with the full value on
    // hover so one wide cell can't blow out the table.
    if (s.size() > 76)
        return "<span title=\"" + web_ui_escape(s) + "\">" +
               web_ui_escape(s.substr(0, 76)) + "\xE2\x80\xA6</span>";
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
    // Icon semantics follow the Studio design contract (one icon = one meaning): home = Studio
    // home, folder = Projects, database = data, book/plug/brackets = the documentation pages.
    if (name == "overview" || name == "home")
        return svg("<path d='M3.5 10.5 12 3.5l8.5 7'/><path d='M5.5 9v10.5a1 1 0 0 0 1 1h11a1 1 0 0 0 1-1V9'/>"
                   "<path d='M9.8 20.5v-6h4.4v6'/>");
    if (name == "grid")
        return svg("<rect x='3' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='3' width='7' height='7' rx='1.5'/>"
                   "<rect x='3' y='14' width='7' height='7' rx='1.5'/><rect x='14' y='14' width='7' height='7' rx='1.5'/>");
    if (name == "docs")
        return svg("<path d='M4 4.5A2 2 0 0 1 6 2.5h13.5v17H6a2 2 0 0 0-2 2z'/>"
                   "<path d='M4 19.5A2 2 0 0 1 6 17.5h13.5M8 7h7M8 10.5h5'/>");
    if (name == "mcp")
        return svg("<path d='M9 7V3.5M15 7V3.5'/><path d='M6.5 7h11v4a5.5 5.5 0 0 1-11 0z'/>"
                   "<path d='M12 16.5v4'/>");
    if (name == "api")
        return svg("<path d='m8 7.5-4.5 4.5L8 16.5M16 7.5l4.5 4.5L16 16.5'/><path d='m13.3 5-2.6 14'/>");
    if (name == "bell")
        return svg("<path d='M18 8.5a6 6 0 0 0-12 0c0 6-2.5 7.5-2.5 7.5h17S18 14.5 18 8.5'/>"
                   "<path d='M10 19.5a2.2 2.2 0 0 0 4 0'/>");
    if (name == "moon")
        return svg("<path d='M20.5 14.5A8.5 8.5 0 1 1 9.5 3.5a7 7 0 0 0 11 11z'/>");
    if (name == "chatbot")
        return svg("<rect x='4' y='7' width='16' height='11' rx='2.5'/><path d='M12 7V4M9 4h6'/>"
                   "<path d='M9 11.5v1.6M15 11.5v1.6'/><path d='M2 11v4M22 11v4'/>");
    if (name == "connections") return web_ui_icon("api");
    if (name == "agents") return web_ui_icon("chatbot");
    if (name == "chat")
        return svg("<path d='M4 5.5h16a1 1 0 0 1 1 1V16a1 1 0 0 1-1 1H9l-4.5 3.5V6.5a1 1 0 0 1 1-1z'/>"
                   "<path d='M8 10h8M8 13h5'/>");
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
    if (name == "dataset" || name == "datasets" || name == "database" || name == "databases" ||
        name == "datagen")
        return svg("<ellipse cx='12' cy='5.5' rx='7.5' ry='2.8'/>"
                   "<path d='M4.5 5.5v13c0 1.55 3.36 2.8 7.5 2.8s7.5-1.25 7.5-2.8v-13'/>"
                   "<path d='M4.5 12c0 1.55 3.36 2.8 7.5 2.8s7.5-1.25 7.5-2.8'/>");
    if (name == "index" || name == "indexes")
        return svg("<path d='M4 5.5h11M4 10h11M4 14.5h6'/>"
                   "<circle cx='16.5' cy='16.5' r='3'/><path d='m18.8 18.8 2.2 2.2'/>");
    if (name == "search")
        return svg("<circle cx='10.5' cy='10.5' r='6.5'/><path d='m15.4 15.4 5.1 5.1'/>");
    if (name == "deployment" || name == "deployments")
        return svg("<path d='M12 2.5 20.5 7v10L12 21.5 3.5 17V7z'/><path d='m8 12 3 3 5-6'/>");
    if (name == "model") return web_ui_icon("models");
    if (name == "node") return web_ui_icon("nodes");
    if (name == "user" || name == "users" || name == "member" || name == "principal")
        return svg("<circle cx='12' cy='8' r='3.6'/><path d='M5 20a7 7 0 0 1 14 0'/>");
    if (name == "role" || name == "roles")
        return svg("<circle cx='11' cy='8' r='3.3'/><path d='M4.7 20a6.3 6.3 0 0 1 12.6 0'/>"
                   "<path d='m17.5 5 1.4 1.4L21.5 3.8'/>");
    if (name == "token" || name == "tokens")
        return svg("<circle cx='8' cy='12' r='3.6'/><path d='M11.5 12H21M18 12v3M21 12v2.6'/>");
    if (name == "trace" || name == "traces" || name == "span")
        return svg("<path d='M4 6.5h8M4 12h12M4 17.5h6'/><circle cx='15' cy='6.5' r='1.5'/>"
                   "<circle cx='19' cy='12' r='1.5'/><circle cx='11' cy='17.5' r='1.5'/>");
    if (name == "audit")
        return svg("<path d='M6 3.5h8l5 5V20a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4.5a1 1 0 0 1 1-1z'/>"
                   "<path d='M14 3.5V8.5h5M8.5 13.5l2 2 3.5-4'/>");
    if (name == "consent")
        return svg("<path d='M12 2.5 20 6v6c0 5-3.4 8-8 9.5C8 20 4.6 17 4.6 12V6z'/>"
                   "<path d='m9 12 2.2 2.2L15.5 10'/>");
    if (name == "policy" || name == "policies" || name == "metric")
        return svg("<rect x='4' y='4' width='16' height='16' rx='2.2'/><path d='M8 15.5v-3.5M12 15.5V9M16 15.5v-5.5'/>");
    if (name == "crash")
        return svg("<path d='M10.3 3.2 2.4 17a1.6 1.6 0 0 0 1.4 2.4h16.4A1.6 1.6 0 0 0 21.6 17L13.7 3.2a1.6 1.6 0 0 0-2.8 0z'/>"
                   "<path d='M12 9v4.5M12 17h.01'/>");
    return svg("<circle cx='12' cy='12' r='3'/>");  // generic — never blank
}

std::string web_ui_login(std::string_view error) {
    std::string out = head("Orama Studio \xE2\x80\x94 sign in");
    out += "<div class=\"loginwrap\"><form class=\"logincard\" method=\"post\" action=\"/login\">";
    out += "<div class=\"brand big\"><span class=\"live\"></span>Orama Studio</div>";
    out += "<p class=\"sub\">Sign in with an admin bearer token. It is kept only as an HttpOnly "
           "session cookie for this browser and validated on every request.</p>";
    if (!error.empty()) out += "<div class=\"err-box\">" + web_ui_escape(error) + "</div>";
    out += "<input type=\"password\" name=\"token\" placeholder=\"bearer token\" autocomplete=\"off\" autofocus>";
    out += "<button class=\"btn btn-primary\" type=\"submit\">Sign in</button>";
    out += "</form></div></body></html>";
    return out;
}

std::string web_ui_shell(std::string_view active, std::string_view user, std::string_view meta,
                         std::string_view content, long long project_count,
                         std::string_view assist_agent) {
    // The Studio shell (#874): the five sidebar groups from the Orama Studio design —
    // Workspace / Operate / Developer / Documentation / Advanced. Every destination maps to a
    // capability or read view the engine actually serves; infrastructure is demoted to Advanced.
    // Nav keys (the ?view= values) are stable — only grouping and labels follow the design.
    auto nav_title = [](std::string_view key) -> std::string_view {
        if (key == "overview") return "Studio home \xE2\x80\x94 cluster health + counts at a glance";
        if (key == "cluster") return "Infrastructure \xE2\x80\x94 nodes, metrics, and deployments; add capacity";
        if (key == "projects") return "AI projects: intent \xE2\x86\x92 Pipeline \xE2\x86\x92 Verification \xE2\x86\x92 deploy \xE2\x86\x92 run";
        if (key == "data") return "Data library \xE2\x80\x94 databases, tables, rows, and files";
        if (key == "models") return "Trained models + serving";
        if (key == "agents") return "Served agents: a model + prompt + tools you can chat with";
        if (key == "chat") return "Talk to a selected agent, with tool-calling";
        if (key == "datagen") return "Generate + gate training datasets natively (grounded in the solutions plan)";
        if (key == "connections") return "Choose a connection path \xE2\x80\x94 API, MCP, or CLI \xE2\x80\x94 and the saved source connections";
        if (key == "pipeline") return "Runs and activity \xE2\x80\x94 Pipelines, deployments, runs, and jobs";
        if (key == "observability") return "Traces, spans, and crashes";
        if (key == "governance") return "Users, roles, policies, tokens, consent, and audit";
        if (key == "shell") return "SQL + Python REPL, auto-detected";
        if (key == "notebooks") return "SQL / Python cells, saved in your browser";
        return "";
    };
    auto link = [&](std::string_view key, std::string_view label,
                    std::string_view badge = {}) -> std::string {
        std::string anchor = "<a title=\"";
        anchor += web_ui_escape(nav_title(key));
        anchor += "\" class=\"";
        if (key == active) anchor += "active";
        anchor += "\" href=\"/?view=";
        anchor += key;
        anchor += "\">";
        anchor += web_ui_icon(key);  // nav keys map 1:1 to icon names
        anchor += "<span>";
        anchor += label;
        anchor += "</span>";
        if (!badge.empty()) {
            anchor += "<span class=\"badge\">";
            anchor += web_ui_escape(badge);
            anchor += "</span>";
        }
        anchor += "</a>";
        return anchor;
    };
    // Server-rendered documentation pages (public discovery docs, real routes).
    auto doc_link = [&](std::string_view href, std::string_view icon, std::string_view label,
                        std::string_view title) -> std::string {
        std::string anchor = "<a title=\"";
        anchor += web_ui_escape(title);
        anchor += "\" href=\"";
        anchor += href;
        anchor += "\">";
        anchor += web_ui_icon(icon);
        anchor += "<span>";
        anchor += label;
        anchor += "</span></a>";
        return anchor;
    };
    // The topbar page title, per view (July 24): the lockup, then the current
    // page name with the Connected pill.
    auto page_title = [](std::string_view key) -> std::string_view {
        if (key == "overview") return "Studio home";
        if (key == "projects") return "Projects";
        if (key == "data") return "Data";
        if (key == "models") return "Models";
        if (key == "pipeline") return "Operate";
        if (key == "observability") return "Observability";
        if (key == "governance") return "Governance";
        if (key == "cluster") return "Infrastructure";
        if (key == "connections") return "Connections";
        if (key == "shell") return "Shell";
        if (key == "notebooks") return "Notebooks";
        if (key == "agents") return "Agents";
        if (key == "chat") return "Chat";
        if (key == "datagen") return "Data generators";
        if (key == "console") return "Console";
        return "Orama Studio";
    };
    std::string out = head("Orama Studio");
    out += "<div class=\"top\"><div class=\"brand\"><span class=\"live\"></span>Orama Studio</div>";
    out += "<span class=\"pagetitle\">" + std::string(page_title(active)) +
           "<span class=\"connpill ok\">connected</span></span><span class=\"spacer\"></span>";
    out += "<a class=\"util\" title=\"Search\" href=\"/?view=data\">" + web_ui_icon("search") + "</a>";
    out += "<a class=\"util\" title=\"Get help\" href=\"/llms.txt\">" + web_ui_icon("docs") + "</a>";
    out += "<a class=\"util\" title=\"Runs and activity\" href=\"/?view=pipeline\">" +
           web_ui_icon("bell") + "</a>";
    // The theme toggle: light → dark → back to following the system. Pure
    // presentation, stored locally; the pre-paint pin in head() applies it.
    out += "<button class=\"util\" id=\"themetoggle\" title=\"Theme\" type=\"button\">" +
           web_ui_icon("moon") + "</button>";
    // The Ask Orama rail toggle (the drawer is closed by default, July 24).
    out += "<button class=\"util\" id=\"railtoggle\" title=\"Ask Orama\" type=\"button\">" +
           web_ui_icon("chatbot") + "</button></div>";
    out += "<div class=\"layout withrail\"><nav class=\"side\">";
    out += "<div class=\"group\">Workspace</div>" + link("overview", "Home") +
           link("projects", "Projects",
                project_count >= 0 ? std::to_string(project_count) : std::string()) +
           link("data", "Data library");
    out += "<div class=\"group\">Operate</div>" + link("pipeline", "Runs and activity") +
           link("observability", "Observability");
    out += "<div class=\"group\">Developer</div>" + link("connections", "Connections") +
           link("models", "Models") + link("shell", "Shell") + link("notebooks", "Notebooks") +
           link("agents", "Agents") + link("chat", "Chat") + link("datagen", "Data generators");
    out += "<div class=\"group\">Documentation</div>" +
           doc_link("/llms.txt", "docs", "Overview", "llms.txt \xE2\x80\x94 the documentation index") +
           doc_link("/mcp.md", "mcp", "MCP", "Connect an MCP client to this node") +
           doc_link("/v1/openapi.json", "api", "API", "The OpenAPI description of the REST surface");
    out += "<div class=\"group\">Advanced</div>" + link("governance", "Governance") +
           link("cluster", "Infrastructure");
    // The signed-in identity card pins to the sidebar's bottom (the design's
    // profile row); sign-out lives here, not in the topbar. The second line
    // carries the node meta the topbar gave up.
    if (!user.empty()) {
        const char initial = std::toupper(static_cast<unsigned char>(user.front()));
        out += "<div class=\"sideuser\"><span class=\"avatar\">";
        out += initial;
        out += "</span><span class=\"who\"><b>" + web_ui_escape(user) + "</b><span>" +
               web_ui_escape(meta) +
               "</span></span>"
               "<form class=\"inline\" method=\"post\" action=\"/logout\">"
               "<button class=\"btn small\">Sign out</button></form></div>";
    }
    out += "</nav><main class=\"main\">";
    out += content;
    out += "</main>";
    // The Ask Orama rail — the design's collaborator column, backed by the
    // node's served agent through POST /assist → the `chat` capability. No
    // agent → the honest empty state (never a mock conversation).
    out += "<aside class=\"rail\"><div class=\"railhead\"><span class=\"rl\">Studio "
           "collaborator</span><b>Ask Orama</b></div>";
    if (assist_agent.empty()) {
        out += "<div class=\"railnote\">Every reply comes from an agent served by this "
               "node, and every call is audited.</div>";
        out += "<div class=\"railempty\">No served agent on this node yet. Create one in "
               "<a href=\"/?view=agents\">Agents</a> and Ask Orama comes to life here.</div>";
    } else {
        out += "<div class=\"railnote\">Replies come from this node's served agent \xC2\xB7 "
               "every call is audited and traced.</div>";
        out += "<div class=\"railbubble\">I can explain the current Studio state, or draft "
               "the intent for a new Project \xE2\x80\x94 ask below.</div>";
        out += "<div class=\"railchat\" id=\"railchat\"></div>";
        out += "<div class=\"railask\"><label for=\"railq\">Ask in Studio</label>"
               "<div class=\"rrow\"><textarea id=\"railq\" rows=\"1\" placeholder=\"Ask "
               "about Studio\xE2\x80\xA6\"></textarea>"
               "<button class=\"btn btn-primary\" id=\"railsend\" type=\"button\">" +
               web_ui_icon("play") + "</button></div></div>";
        out += "<script>(function(){var agent=" +
               nlohmann::json(std::string(assist_agent)).dump() +
               R"JS(;var chat=document.getElementById('railchat'),q=document.getElementById('railq'),send=document.getElementById('railsend');
function esc(s){return String(s==null?'':s);}
function bubble(cls,text){var d=document.createElement('div');d.className='rmsg '+cls;d.textContent=esc(text);chat.appendChild(d);chat.scrollTop=chat.scrollHeight;return d;}
var busy=false;
async function ask(){var m=q.value.trim();if(!m||busy)return;busy=true;q.value='';bubble('rme',m);
  var t=bubble('ragent rthinking','thinking…');
  try{var res=await fetch('/assist',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({agent:agent,message:m})});
    var j=await res.json();
    var v=j&&j.value?j.value:{};
    t.classList.remove('rthinking');
    t.textContent=esc(j&&j.ok?(v.reply||v.message||v.text||JSON.stringify(v)):('error: '+(j&&j.error?j.error:'request failed')));
  }catch(e){t.classList.remove('rthinking');t.textContent='error: '+e;}
  busy=false;chat.scrollTop=chat.scrollHeight;}
send.onclick=ask;
q.addEventListener('keydown',function(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();ask();}});
})();</script>)JS";
    }
    out += "</aside>";
    // The theme toggle cycle (system → light → dark → system) and the rail
    // drawer toggle; both persist locally.
    out += R"JS(<script>(function(){var b=document.getElementById('themetoggle');if(b)
b.onclick=function(){var cur;try{cur=localStorage.getItem('zetta-theme');}catch(e){}
  var next=cur==='light'?'dark':(cur==='dark'?null:'light');
  try{if(next)localStorage.setItem('zetta-theme',next);else localStorage.removeItem('zetta-theme');}catch(e){}
  if(next)document.documentElement.dataset.theme=next;else delete document.documentElement.dataset.theme;};
var r=document.getElementById('railtoggle');
function railSync(){var open=document.documentElement.dataset.rail==='open';if(r)r.classList.toggle('active',open);}
if(r)r.onclick=function(){var open=document.documentElement.dataset.rail==='open';
  if(open){delete document.documentElement.dataset.rail;try{localStorage.removeItem('zetta-rail');}catch(e){}}
  else{document.documentElement.dataset.rail='open';try{localStorage.setItem('zetta-rail','open');}catch(e){}}
  railSync();};
railSync();
})();</script>)JS";
    out += "</div></body></html>";
    return out;
}

// Pick a section icon from the panel title's keywords (keeps the panel signature unchanged).
std::string panel_icon_for(std::string_view title) {
    const std::string t = lower(std::string(title));
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    if (has("search")) return web_ui_icon("search");
    if (has("index")) return web_ui_icon("index");
    if (has("dataset") || has("data") || has("explorer") || has("file"))
        return web_ui_icon("dataset");
    if (has("node") || has("cluster") || has("member")) return web_ui_icon("cluster");
    if (has("model")) return web_ui_icon("models");
    if (has("deployment")) return web_ui_icon("deployment");
    if (has("graph") || has("run") || has("job") || has("pipeline")) return web_ui_icon("pipeline");
    if (has("trace") || has("span")) return web_ui_icon("trace");
    if (has("crash")) return web_ui_icon("crash");
    if (has("metric") || has("observ")) return web_ui_icon("observability");
    if (has("user")) return web_ui_icon("user");
    if (has("role")) return web_ui_icon("role");
    if (has("token")) return web_ui_icon("token");
    if (has("polic")) return web_ui_icon("policy");
    if (has("consent")) return web_ui_icon("consent");
    if (has("audit") || has("govern")) return web_ui_icon("audit");
    return web_ui_icon("grid");
}

std::optional<std::string> web_ui_webapp_path(std::string_view url_path) {
    std::string rel(url_path);
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    if (rel.empty()) return std::string("index.html");
    // Reject anything that could escape the bundle root. Encoded traversal is
    // already decoded by the router; backslashes never appear in bundle paths.
    if (rel.find("..") != std::string::npos) return std::nullopt;
    if (rel.find('\\') != std::string::npos) return std::nullopt;
    if (rel.find('\0') != std::string::npos) return std::nullopt;
    return rel;
}

std::string_view web_ui_content_type(std::string_view path) {
    const auto dot = path.rfind('.');
    const std::string_view ext = dot == std::string_view::npos ? "" : path.substr(dot + 1);
    if (ext == "html") return "text/html; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "json" || ext == "map") return "application/json";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff2") return "font/woff2";
    if (ext == "txt") return "text/plain; charset=utf-8";
    return "text/html; charset=utf-8";
}

std::string web_ui_page_hero(std::string_view eyebrow, std::string_view title,
                             std::string_view sub, std::string_view meta_html,
                             std::string_view actions_html) {
    std::string out = "<div class=\"hero\"><div class=\"hbody\">";
    out += "<div class=\"heyebrow\">" + web_ui_escape(eyebrow) + "</div>";
    out += "<h1>" + web_ui_escape(title) + "</h1>";
    if (!sub.empty()) out += "<div class=\"hsub\">" + web_ui_escape(sub) + "</div>";
    if (!meta_html.empty()) out += "<div class=\"hmeta\">" + std::string(meta_html) + "</div>";
    out += "</div>";
    if (!actions_html.empty()) out += "<div class=\"hactions\">" + std::string(actions_html) + "</div>";
    out += "</div>";
    return out;
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
    struct Card { const char* label; const char* icon; const char* view; std::string value; };
    const std::array<Card, 6> cards = {{{"Nodes", "nodes", "cluster", fmt_int(nodes)},
                                        {"Databases", "data", "data", count("databases")},
                                        {"Projects", "projects", "projects", count("projects")},
                                        {"Models", "models", "models", count("models")},
                                        {"Runs", "runs", "pipeline", count("runs")},
                                        {"Jobs", "jobs", "pipeline", count("jobs")}}};
    std::string out = "<div class=\"cards\">";
    for (const auto& c : cards)
        out += "<a class=\"card\" href=\"/?view=" + std::string(c.view) +
               "\"><div class=\"chd\"><span class=\"cico\">" + web_ui_icon(c.icon) +
               "</span></div><div class=\"num\">" + c.value + "</div><div class=\"lbl\">" +
               c.label + "</div></a>";
    out += "</div>";
    return out;
}

std::string web_ui_stylesheet() { return std::string(kCss); }

std::string web_ui_favicon() {
    return R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><rect width="32" height="32" rx="7" fill="#0F62FE"/><text x="16" y="23" font-family="'IBM Plex Sans',-apple-system,Helvetica,Arial,sans-serif" font-size="20" font-weight="600" fill="#fff" text-anchor="middle">O</text></svg>)SVG";
}

namespace {
// Shared client helpers for the interactive tools (Shell + Notebooks): POST a command to the
// cookie-authed /shell endpoint and format a SQL result as a monospace text table.
constexpr std::string_view kSharedShellJs = R"JS(
function zEsc(s){return String(s==null?'':s).replace(/[&<>]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
async function zExec(q){var t0=performance.now();var res=await fetch('/shell',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'q='+encodeURIComponent(q)});var j=await res.json();if(j&&typeof j==='object')j.__ms=Math.round(performance.now()-t0);return j;}
// The formatted execution time shown bottom-right after each run: the engine's own
// elapsed when it reports it (Python), else the measured round-trip. Returns just the
// label — the caller places it OUTSIDE the horizontally-scrolling output so it stays
// pinned to the right edge (doesn't drift when a wide table is scrolled).
function zTimeLabel(r){
  if(!r) return '';
  return r.elapsed?r.elapsed:(typeof r.__ms==='number'?(r.__ms<1000?r.__ms+' ms':(r.__ms/1000).toFixed(2)+' s'):'');
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
        "<div class=\"term\" id=\"term\"><div class=\"q\">Orama Studio console \xC2\xB7 type SQL or Python \xC2\xB7 <b>/</b> for commands.</div></div>"
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
    try{ var res=await zExec(c); o.innerHTML=zFmt(res); var lbl=zTimeLabel(res); if(lbl) b.insertAdjacentHTML('beforeend','<div class="exectime">'+zEsc(lbl)+'</div>'); }catch(e){ o.innerHTML='<pre class="txt err">error: '+zEsc(''+e)+'</pre>'; }
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
  async function runCell(i,d){var o=d.querySelector('.out');o.innerHTML='<span class="txt">running…</span>';try{var res=await zExec(book[i].src);book[i].out=zFmt(res);o.innerHTML=book[i].out;var tt=d.querySelector('.exectime');if(tt)tt.textContent=zTimeLabel(res);}catch(e){o.innerHTML='<pre class="txt err">error: '+zEsc(''+e)+'</pre>';book[i].out='';}}
  function render(){
    cellsEl.innerHTML='';
    book.forEach(function(cell,i){
      var d=document.createElement('div');d.className='cell';
      d.innerHTML='<div class="cellbar"><span class="q mono">['+(i+1)+']</span><button class="btn small run">Run</button><button class="btn small del">✕</button></div><textarea class="src" spellcheck="false"></textarea><div class="out">'+(cell.out||'')+'</div><div class="exectime"></div>';
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
