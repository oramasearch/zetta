#include "zetta/api/web_ui.hpp"

namespace zetta::api {

namespace {

// One self-contained page: HTML + CSS + vanilla JS, no external assets (air-gapped nodes have no
// egress, so nothing may load from a CDN). The JS fetches the node's OWN read surface over the same
// origin — server_info, the system.* views via /v1/sql, and the OpenAPI registry — and renders it as
// a read-only dashboard. It prompts for the bearer token and keeps it in sessionStorage. The page is
// STRICTLY A RENDERING of the existing surfaces (#647): it never introduces a behavior path. Styling
// follows the Orama design system: light surfaces, ink text, coral used surgically (one brand mark),
// semantic green/amber/red status as a dot + word, Geist/Geist Mono with a system-font fallback.
constexpr std::string_view kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Orama Zetta</title>
<style>
:root{
  --coral:#E2545A; --coral-hover:#D44349; --coral-wash:rgba(226,84,90,.06); --coral-line:rgba(226,84,90,.30);
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
html,body{height:100%;margin:0;}
body{display:grid;grid-template-columns:208px 1fr;grid-template-rows:52px 1fr;
  grid-template-areas:"top top" "nav main";
  background:var(--canvas);color:var(--ink-primary);
  font:14px/1.55 var(--font-sans);-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale;}
.mono,code,pre{font-family:var(--font-mono);font-variant-numeric:tabular-nums;}
.q{color:var(--ink-quiet);}
a{color:var(--ink-secondary);}

/* Top bar */
.top{grid-area:top;display:flex;align-items:center;gap:14px;padding:0 16px;
  background:var(--white);border-bottom:1px solid var(--rule-hair);}
.brand{display:flex;align-items:center;gap:8px;font-size:16px;font-weight:500;letter-spacing:-.01em;color:var(--ink-primary);}
.brand .live{width:8px;height:8px;border-radius:50%;background:var(--coral);}
.spacer{flex:1;}
.nodemeta{font:11px/1 var(--font-mono);color:var(--ink-quiet);}
.connpill{display:inline-flex;align-items:center;gap:6px;font:11px/1 var(--font-mono);font-weight:500;
  text-transform:uppercase;letter-spacing:.04em;padding:4px 8px;border-radius:8px;background:var(--wash-hover);color:var(--ink-quiet);}
.connpill.ok{color:var(--ok-fg);background:var(--ok-bg);}
.connpill.err{color:var(--err-fg);background:var(--err-bg);}

/* Sidebar nav */
.nav{grid-area:nav;background:var(--sidebar);border-right:1px solid var(--rule-hair);padding:10px 8px;overflow:auto;}
.nav a{display:block;padding:7px 10px;margin-bottom:1px;border-radius:8px;color:var(--ink-secondary);
  text-decoration:none;font-size:13px;cursor:pointer;user-select:none;}
.nav a:hover{background:var(--wash-hover);}
.nav a.active{background:var(--white);color:var(--ink-primary);font-weight:500;}
.navlabel{font:11px/1 var(--font-mono);text-transform:uppercase;letter-spacing:.08em;color:var(--ink-quiet);
  padding:12px 10px 6px;}

/* Main */
.main{grid-area:main;overflow:auto;padding:20px 24px 48px;}
.panel{background:var(--white);border:1px solid var(--rule-card);border-radius:12px;padding:16px;margin-bottom:16px;}
.phead{display:flex;align-items:baseline;gap:10px;margin-bottom:12px;}
.phead h2{margin:0;font-size:15px;font-weight:500;color:var(--ink-primary);}
.psub{font:11px/1 var(--font-sans);color:var(--ink-muted);}
.empty,.loading{color:var(--ink-quiet);font-size:13px;padding:8px 0;}
.err-box{color:var(--err-fg);background:var(--err-bg);border-radius:8px;padding:10px 12px;font:12px/1.5 var(--font-mono);}

/* Cards */
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:12px;}
.card{background:var(--canvas);border:1px solid var(--rule-hair);border-radius:12px;padding:14px;}
.card .num{font:26px/1 var(--font-mono);color:var(--ink-primary);}
.card .lbl{margin-top:6px;font-size:11px;font-weight:500;text-transform:uppercase;letter-spacing:.04em;color:var(--ink-muted);}
.metaline{margin-top:12px;font-size:11px;}

/* Gauges */
.gauges{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:14px;}
.gauge .glabel{display:flex;justify-content:space-between;font-size:12px;color:var(--ink-secondary);margin-bottom:5px;}
.gbar{height:6px;border-radius:6px;background:var(--wash-selected);overflow:hidden;}
.gfill{height:100%;border-radius:6px;background:var(--dot-active);}
.gfill.warn{background:var(--warn-fg);} .gfill.err{background:var(--err-fg);}

/* Tables */
.twrap{overflow-x:auto;}
table{width:100%;border-collapse:collapse;font-size:13px;}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--rule-hair);vertical-align:top;white-space:nowrap;}
th{font:11px/1 var(--font-sans);font-weight:500;text-transform:uppercase;letter-spacing:.04em;color:var(--ink-muted);}
td{color:var(--ink-secondary);}
tbody tr:hover{background:var(--wash-hover);}
td .mono,td.mono{font-family:var(--font-mono);color:var(--ink-primary);font-size:12px;}
code.j{font-size:11px;color:var(--ink-muted);}
.rowcount{margin-top:8px;font-size:11px;}

/* Status pill + dot */
.pill{display:inline-flex;align-items:center;font:11px/1 var(--font-mono);font-weight:500;text-transform:uppercase;
  letter-spacing:.04em;border-radius:8px;padding:3px 7px;color:var(--ink-secondary);background:var(--wash-hover);}
.pill.ok{color:var(--ok-fg);background:var(--ok-bg);} .pill.warn{color:var(--warn-fg);background:var(--warn-bg);}
.pill.err{color:var(--err-fg);background:var(--err-bg);}
.status{display:inline-flex;align-items:center;gap:6px;color:var(--ink-secondary);}
.dot{width:8px;height:8px;border-radius:50%;background:var(--dot-inactive);}
.status.ok .dot{background:var(--dot-active);}
.method{font:11px/1 var(--font-mono);font-weight:500;color:var(--ink-secondary);background:var(--wash-hover);
  border:1px solid var(--rule-card);border-radius:8px;padding:2px 6px;}
.chips{display:flex;flex-wrap:wrap;gap:6px;}
.chip{font:12px/1 var(--font-mono);color:var(--ink-primary);background:var(--canvas);border:1px solid var(--rule-card);
  border-radius:8px;padding:5px 9px;}
a.drill{color:var(--ink-primary);text-decoration:none;border-bottom:1px dotted var(--rule-emph);cursor:pointer;}
a.drill:hover{color:var(--coral);}

/* Buttons */
.btn{font:13px/1 var(--font-sans);color:var(--ink-primary);background:var(--wash-hover);border:1px solid var(--rule-emph);
  border-radius:8px;padding:7px 12px;cursor:pointer;}
.btn:hover{background:var(--wash-selected);}
.btn.small{padding:4px 9px;font-size:12px;}
.btn-primary{background:var(--ink-primary);color:var(--white);border-color:var(--ink-primary);}
.btn-primary:hover{background:#000;}

/* Tabs */
.tabs{display:flex;flex-wrap:wrap;gap:4px;margin-bottom:12px;border-bottom:1px solid var(--rule-hair);padding-bottom:8px;}
.tab{font:12px/1 var(--font-sans);color:var(--ink-secondary);background:none;border:1px solid transparent;
  border-radius:8px;padding:6px 10px;cursor:pointer;}
.tab:hover{background:var(--wash-hover);}
.tab.active{background:var(--white);border-color:var(--rule-card);color:var(--ink-primary);font-weight:500;}

/* SQL */
.sqlbox textarea{width:100%;min-height:96px;resize:vertical;padding:10px 12px;border:1px solid var(--rule-emph);
  border-radius:8px;background:var(--canvas);color:var(--ink-primary);font:12px/1.5 var(--font-mono);}
.sqlbar{display:flex;align-items:center;gap:10px;margin:10px 0;}
.msgs{color:var(--ink-muted);font-size:11px;margin-bottom:8px;}
.apilinks{margin-bottom:12px;font:12px/1 var(--font-mono);}

/* Drawer + gate overlays */
.overlay{position:fixed;inset:0;background:rgba(28,28,26,.28);display:none;z-index:20;}
.overlay.open{display:block;}
.drawer{position:fixed;top:0;right:0;bottom:0;width:min(560px,92vw);background:var(--white);
  border-left:1px solid var(--rule-emph);transform:translateX(100%);transition:transform .16s ease;z-index:21;
  display:flex;flex-direction:column;}
.drawer.open{transform:none;}
.dhead{display:flex;align-items:center;gap:10px;padding:14px 16px;border-bottom:1px solid var(--rule-hair);}
.dhead h3{margin:0;font-size:14px;font-weight:500;overflow:hidden;text-overflow:ellipsis;}
.dbody{overflow:auto;padding:16px;flex:1;}
pre.json{margin:0;font-size:12px;line-height:1.5;color:var(--ink-primary);white-space:pre-wrap;word-break:break-word;}
.gate{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(28,28,26,.3);z-index:30;}
.gate.open{display:flex;}
.gatecard{background:var(--white);border:1px solid var(--rule-emph);border-radius:12px;padding:22px;width:min(420px,92vw);}
.gatecard h3{margin:0 0 6px;font-size:15px;font-weight:500;}
.gatecard p{margin:0 0 14px;font-size:12px;color:var(--ink-muted);}
.gatecard input{width:100%;padding:9px 11px;border:1px solid var(--rule-emph);border-radius:8px;
  font:12px/1 var(--font-mono);background:var(--canvas);margin-bottom:12px;}
.gaterow{display:flex;gap:8px;justify-content:flex-end;}
</style>
</head>
<body>
<div class="top">
  <div class="brand" id="brand"><span class="live"></span>Orama Zetta</div>
  <span class="connpill" id="conn"><span class="ctext">connecting</span></span>
  <span class="spacer"></span>
  <span class="nodemeta" id="nodeMeta"></span>
  <button class="btn small" id="tokenBtn">Token</button>
  <button class="btn small" id="refreshBtn">Refresh</button>
</div>
<nav class="nav" id="nav"></nav>
<main class="main" id="main"><div class="loading">Loading...</div></main>

<div class="overlay" id="drawerOverlay"></div>
<aside class="drawer" id="drawer">
  <div class="dhead"><h3 id="drawerTitle">Detail</h3><span class="spacer"></span><button class="btn small" id="drawerClose">Close</button></div>
  <div class="dbody" id="drawerBody"></div>
</aside>

<div class="gate" id="gate">
  <div class="gatecard">
    <h3>Connect to this node</h3>
    <p>Paste an admin bearer token. It is kept only in this tab (sessionStorage) and sent as <span class="mono">Authorization: Bearer</span> on each call.</p>
    <input id="tokenInput" type="password" placeholder="bearer token" autocomplete="off" spellcheck="false">
    <div class="gaterow"><button class="btn" id="gateSkip">Skip</button><button class="btn btn-primary" id="gateConnect">Connect</button></div>
  </div>
</div>

<script>
const TK='zetta_token';
let token = sessionStorage.getItem(TK) || '';
let view = (location.hash||'#overview').slice(1);
const $ = s => document.querySelector(s);
const esc = s => String(s==null?'':s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

const NAV = [
  ['Cluster', [['overview','Overview'],['cluster','Cluster']]],
  ['Workloads', [['data','Data'],['models','Models'],['pipeline','Pipeline']]],
  ['Operate', [['observability','Observability'],['governance','Governance']]],
  ['Explore', [['sql','SQL'],['api','API']]],
];
function buildNav(){
  $('#nav').innerHTML = NAV.map(([g,items])=>`<div class="navlabel">${g}</div>`+
    items.map(([k,l])=>`<a data-view="${k}">${l}</a>`).join('')).join('');
}

/* ---- fetch layer ---- */
function setConn(ok,msg){ const c=$('#conn'); c.className='connpill '+(ok?'ok':'err'); c.querySelector('.ctext').textContent = ok?'connected':(msg||'offline'); }
async function raw(path){
  const h={'Accept':'application/json'};
  if(token) h['Authorization']='Bearer '+token;
  let r;
  try{ r = await fetch(path,{headers:h,cache:'no-store'}); }
  catch(e){ setConn(false,'network'); throw e; }
  if(r.status===401){ setConn(false,'token required'); openGate(); throw new Error('401'); }
  setConn(true);
  const ct = r.headers.get('content-type')||'';
  return ct.indexOf('json')>=0 ? r.json() : r.text();
}
async function call(path){ const j = await raw(path); if(j && j.ok===false) throw new Error(j.error?j.error.message:'error'); return (j && typeof j==='object' && 'value' in j) ? j.value : j; }
const sql = stmt => call('/v1/sql?statement='+encodeURIComponent(stmt));

/* ---- formatting ---- */
function human(n){ n=+n; if(!isFinite(n)) return '.'; const u=['B','KB','MB','GB','TB','PB']; let i=0; while(n>=1024&&i<u.length-1){n/=1024;i++;} return (i?n.toFixed(1):n)+' '+u[i]; }
const OK=/^(live|running|ready|pass|passed|ok|approved|healthy|active|exposed|executed|done|complete|completed|succeeded|in.?sync|true|yes)$/i;
const WARN=/^(sync|syncing|pending|progress|progressing|stale|in.?flight|attention|warn|warning|queued|starting|uncertain|busy)$/i;
const ERR=/^(fail|failed|error|deny|denied|expired|revoked|crash|crashed|false|no|degraded|unreachable|behind|dial.?out.?only|blocked)$/i;
function tone(v){ v=String(v).trim().toLowerCase(); if(ERR.test(v))return'err'; if(WARN.test(v))return'warn'; if(OK.test(v))return'ok'; return ''; }
const pill = v => `<span class="pill ${tone(v)}">${esc(v)}</span>`;
const DRILL = { run_id:{ep:id=>'/v1/get_run?run_id='+encodeURIComponent(id), t:id=>'Run '+id},
                trace_id:{ep:id=>'/v1/get_trace?trace_id='+encodeURIComponent(id), t:id=>'Trace '+id} };
const STATUS=/^(status|verdict|outcome|phase|reachability|state)$/i;
const BOOLC=/(^|_)(in_sync|is_self|exposed|ok|allowed|revoked|present|runnable|authenticated|reachable|partitioned|directly_connected|has_weights|weights_present|block_deploy|valid|unrestricted|gpu_present)$/i;
const BYTES=/(bytes|_size$|^size$|vram|wire_bytes)/i;
const MONOC=/(_id$|^id$|node|address|path|sha256|endpoint|version|hash|trace|span|ref|token|_at$|created|updated|owner|placement|fields|columns|scope|capabilities|permissions|roles)/i;
function fmt(col,v){
  if(v==null||v===''||(Array.isArray(v)&&!v.length)) return '<span class="q">.</span>';
  if(DRILL[col] && v) return `<a class="drill" data-col="${col}" data-id="${esc(v)}">${esc(v)}</a>`;
  if(STATUS.test(col)) return pill(v);
  if(BOOLC.test(col)){ const b=(v===true||v==='true'||v==='t'||v===1); return `<span class="status ${b?'ok':''}"><span class="dot"></span>${b?'yes':'no'}</span>`; }
  if(typeof v==='number') return `<span class="mono">${BYTES.test(col)?human(v):v}</span>`;
  if(Array.isArray(v)) return `<span class="mono">${esc(v.slice(0,6).join(', '))}${v.length>6?' +'+(v.length-6):''}</span>`;
  if(typeof v==='object') return `<code class="j">${esc(JSON.stringify(v)).slice(0,140)}</code>`;
  if(BYTES.test(col)&&/^\d+$/.test(v)) return `<span class="mono">${human(v)}</span>`;
  if(MONOC.test(col)) return `<span class="mono">${esc(v)}</span>`;
  return esc(v);
}
function table(cols,rows){
  if(!rows||!rows.length) return '<div class="empty">No rows.</div>';
  cols = cols || Object.keys(rows[0]);
  return `<div class="twrap"><table><thead><tr>${cols.map(c=>`<th>${esc(c.replace(/_/g,' '))}</th>`).join('')}</tr></thead>`+
    `<tbody>${rows.map(r=>`<tr>${cols.map(c=>`<td>${fmt(c,r[c])}</td>`).join('')}</tr>`).join('')}</tbody></table></div>`;
}
async function sqlTable(stmt){ const v = await sql(stmt); return table(v.columns, v.rows); }
const panel = (title,body,sub)=>`<section class="panel"><div class="phead"><h2>${esc(title)}</h2>${sub?`<span class="psub">${esc(sub)}</span>`:''}</div>${body}</section>`;
async function recent(tbl,cols,n){ const v=await sql('SELECT * FROM '+tbl); const rows=(v.rows||[]).slice(-n).reverse(); return table(cols.filter(c=>(v.columns||[]).indexOf(c)>=0), rows); }

/* ---- views ---- */
function gauge(l,p){ p=Math.max(0,Math.min(100,+p||0)); const t=p>=90?'err':p>=70?'warn':''; return `<div class="gauge"><div class="glabel">${l}<span class="mono">${p.toFixed(0)}%</span></div><div class="gbar"><div class="gfill ${t}" style="width:${p}%"></div></div></div>`; }
async function viewOverview(){
  const i = await call('/v1/server_info');
  const cards=[['Nodes',(i.cluster_members||[]).length],['Databases',i.databases],['Projects',i.projects],['Models',i.models],['Runs',i.runs],['Jobs',i.jobs]];
  let html = panel('Overview',
    `<div class="cards">${cards.map(([l,n])=>`<div class="card"><div class="num">${n==null?'.':n}</div><div class="lbl">${l}</div></div>`).join('')}</div>`+
    `<div class="metaline mono">${esc(i.node_id)} &middot; v${esc(i.version)} &middot; ${(i.cluster_members||[]).length} nodes</div>`);
  try{ const m=await sql('SELECT * FROM system.node_metrics'); const r=(m.rows||[])[0]; if(r) html+=panel('This node',
    `<div class="gauges">${gauge('CPU',r.cpu_percent)}${gauge('Memory',r.memory_percent)}${gauge('Storage',r.storage_percent)}${r.gpu_present?gauge('GPU',r.gpu_percent):''}</div>`); }catch(e){}
  html += panel('Cluster', await sqlTable('SELECT * FROM system.nodes'));
  try{ html += panel('Recent activity', await recent('system.activity',['time','surface','actor','action','target','outcome'],12)); }catch(e){}
  try{ html += panel('Recent runs', await recent('system.runs',['run_id','project','graph','status','updated'],6),'click a run id for detail'); }catch(e){}
  return html;
}
async function viewCluster(){
  return panel('Nodes', await sqlTable('SELECT * FROM system.nodes'),'membership &middot; reachability &middot; sync')+
    panel('Node metrics', await sqlTable('SELECT * FROM system.node_metrics'),'this node')+
    panel('Deployments', await sqlTable('SELECT * FROM system.deployments'));
}
async function viewData(){
  return panel('Datasets', await sqlTable('SELECT * FROM system.datasets'))+
    panel('Indexes', await sqlTable('SELECT * FROM system.indexes'));
}
async function viewModels(){ return panel('Models', await sqlTable('SELECT * FROM system.models')); }
async function viewPipeline(){
  let head='';
  try{ const p=await call('/v1/list_projects'); head=panel('Projects', `<div class="chips">${(p.projects||[]).map(n=>`<span class="chip">${esc(n)}</span>`).join('')||'<span class="q">No projects.</span>'}</div>`); }catch(e){}
  return head + panel('Runs', await sqlTable('SELECT * FROM system.runs'),'click a run id for logs, outputs, lineage')+
    panel('Jobs', await sqlTable('SELECT * FROM system.jobs'));
}
async function viewObs(){
  return panel('Traces', await sqlTable('SELECT * FROM system.traces'),'click a trace id for its spans')+
    panel('Spans', await sqlTable('SELECT * FROM system.spans'))+
    panel('Crashes', await sqlTable('SELECT * FROM system.crashes'));
}
const GOV=[['users','Users'],['roles','Roles'],['user_roles','User roles'],['policies','Policies'],['tokens','Tokens'],['principals','Principals'],['keys','Keys'],['consent_requests','Consent'],['audit','Audit'],['activity','Activity']];
async function viewGov(){
  const active = sessionStorage.getItem('zetta_gov')||'users';
  const tabs = GOV.map(([k,l])=>`<button class="tab ${k===active?'active':''}" data-tab="${k}">${l}</button>`).join('');
  return panel('Governance', `<div class="tabs">${tabs}</div><div id="govbody">${await sqlTable('SELECT * FROM system.'+active)}</div>`);
}
function viewSQL(){
  const last = sessionStorage.getItem('zetta_sql')||'SELECT * FROM system.tables';
  return panel('SQL console',
    `<div class="sqlbox"><textarea id="sqlIn" spellcheck="false">${esc(last)}</textarea>`+
    `<div class="sqlbar"><button class="btn btn-primary" data-act="sql-run">Run</button><span class="q">GET /v1/sql &middot; Postgres subset &middot; the ~30 system.* views are the read model</span></div>`+
    `<div id="sqlOut"></div></div>`);
}
async function viewAPI(){
  const doc = await raw('/v1/openapi.json');
  const rows=[]; const paths=doc.paths||{};
  for(const p of Object.keys(paths)) for(const m of Object.keys(paths[p])){ const op=paths[p][m]||{}; rows.push({m:m.toUpperCase(),p,s:op.summary||op.description||''}); }
  rows.sort((a,b)=> a.p===b.p ? a.m.localeCompare(b.m) : a.p.localeCompare(b.p));
  const body = `<div class="twrap"><table><thead><tr><th>Method</th><th>Path</th><th>Summary</th><th></th></tr></thead><tbody>`+
    rows.map(r=>`<tr><td><span class="method">${r.m}</span></td><td class="mono">${esc(r.p)}</td><td>${esc(r.s)}</td>`+
      `<td>${r.m==='GET'&&r.p.indexOf('{')<0?`<button class="btn small" data-run="${esc(r.p)}">Run</button>`:''}</td></tr>`).join('')+
    `</tbody></table></div>`;
  const links = `<div class="apilinks"><a href="/mcp.md" target="_blank">/mcp.md</a> &middot; <a href="/llms.txt" target="_blank">/llms.txt</a> &middot; <a href="/v1/openapi.json" target="_blank">/v1/openapi.json</a></div>`;
  return panel('API &middot; capability registry', links+body, rows.length+' operations');
}
const VIEWS={overview:viewOverview,cluster:viewCluster,data:viewData,models:viewModels,pipeline:viewPipeline,observability:viewObs,governance:viewGov,sql:viewSQL,api:viewAPI};

/* ---- drawer / gate ---- */
function openDrawer(t,html){ $('#drawerTitle').textContent=t; $('#drawerBody').innerHTML=html; $('#drawer').classList.add('open'); $('#drawerOverlay').classList.add('open'); }
function closeDrawer(){ $('#drawer').classList.remove('open'); $('#drawerOverlay').classList.remove('open'); }
const drawerJSON = v => $('#drawerBody').innerHTML = `<pre class="json">${esc(JSON.stringify(v,null,2))}</pre>`;
function openGate(){ $('#tokenInput').value=token; $('#gate').classList.add('open'); setTimeout(()=>$('#tokenInput').focus(),40); }
function closeGate(){ $('#gate').classList.remove('open'); }

/* ---- render ---- */
async function render(){
  document.querySelectorAll('.nav a').forEach(a=>a.classList.toggle('active',a.dataset.view===view));
  const main=$('#main'); main.innerHTML='<div class="loading">Loading...</div>';
  try{ main.innerHTML = await (VIEWS[view]||viewOverview)(); }
  catch(e){ main.innerHTML = String(e.message).indexOf('401')>=0 ? '<div class="empty">Enter a token to load data.</div>' : `<div class="err-box">${esc(e.message||e)}</div>`; }
}
function setView(v){ view=v; location.hash=v; render(); }
async function ping(){ try{ const i=await call('/v1/server_info'); $('#nodeMeta').textContent = String(i.node_id).slice(0,12)+' · v'+i.version+' · '+((i.cluster_members||[]).length)+' nodes'; }catch(e){} }

/* ---- delegated events ---- */
document.addEventListener('click', async e=>{
  const nav=e.target.closest('.nav a'); if(nav){ setView(nav.dataset.view); return; }
  const drill=e.target.closest('a.drill'); if(drill){ e.preventDefault(); const d=DRILL[drill.dataset.col]; openDrawer(d.t(drill.dataset.id),'<div class="loading">Loading...</div>'); try{ drawerJSON(await call(d.ep(drill.dataset.id))); }catch(err){ $('#drawerBody').innerHTML=`<div class="err-box">${esc(err.message)}</div>`; } return; }
  const tab=e.target.closest('.tab'); if(tab){ const k=tab.dataset.tab; sessionStorage.setItem('zetta_gov',k); document.querySelectorAll('.tab').forEach(t=>t.classList.toggle('active',t.dataset.tab===k)); const b=$('#govbody'); b.innerHTML='<div class="loading">Loading...</div>'; try{ b.innerHTML=await sqlTable('SELECT * FROM system.'+k); }catch(err){ b.innerHTML=`<div class="err-box">${esc(err.message)}</div>`; } return; }
  const runp=e.target.closest('[data-run]'); if(runp){ const p=runp.dataset.run; openDrawer(p,'<div class="loading">Loading...</div>'); try{ drawerJSON(await call(p)); }catch(err){ $('#drawerBody').innerHTML=`<div class="err-box">${esc(err.message)}</div>`; } return; }
  if(e.target.closest('[data-act="sql-run"]')){ const stmt=$('#sqlIn').value; sessionStorage.setItem('zetta_sql',stmt); const out=$('#sqlOut'); out.innerHTML='<div class="loading">Running...</div>'; try{ const v=await sql(stmt); out.innerHTML=(v.messages&&v.messages.length?`<div class="msgs mono">${v.messages.map(esc).join('<br>')}</div>`:'')+table(v.columns,v.rows)+`<div class="q rowcount">${(v.rows||[]).length} rows</div>`; }catch(err){ out.innerHTML=`<div class="err-box">${esc(err.message)}</div>`; } return; }
  if(e.target.id==='drawerClose'||e.target.id==='drawerOverlay'){ closeDrawer(); return; }
  if(e.target.id==='refreshBtn'){ ping(); render(); return; }
  if(e.target.id==='tokenBtn'){ openGate(); return; }
  if(e.target.id==='gateSkip'){ closeGate(); return; }
  if(e.target.id==='gateConnect'){ token=$('#tokenInput').value.trim(); sessionStorage.setItem(TK,token); closeGate(); ping(); render(); return; }
});
window.addEventListener('hashchange', ()=>{ const v=(location.hash||'#overview').slice(1); if(v!==view){ view=v; render(); } });

buildNav();
if(!token) openGate();
ping(); render();
setInterval(()=>{ if(view==='overview' && !$('#gate').classList.contains('open') && !$('#drawer').classList.contains('open')){ ping(); render(); } }, 12000);
</script>
</body>
</html>
)HTML";

}  // namespace

std::string_view web_ui_page() { return kPage; }

}  // namespace zetta::api
