#include "zetta/api/web_ui.hpp"

namespace zetta::api {

namespace {

// One self-contained page: HTML + CSS + vanilla JS, no external assets. The JS fetches the node's own
// REST surface (same origin) and renders it; it prompts for the bearer token and keeps it in
// sessionStorage. Kept deliberately small and dependency-free (straight Beast2-served HTML/JS).
constexpr std::string_view kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Zetta</title>
<style>
  :root{ --bg:#0d0d12; --panel:#16161f; --line:#262632; --fg:#e7e7ef; --dim:#9a9ab0;
         --accent:#7c5cff; --ok:#39d98a; --warn:#ffb020; --err:#ff5470; --mono:ui-monospace,SFMono-Regular,Menlo,monospace; }
  @media (prefers-color-scheme: light){
    :root{ --bg:#f6f6fa; --panel:#fff; --line:#e6e6ee; --fg:#1a1a22; --dim:#6a6a80; }
  }
  *{ box-sizing:border-box; }
  body{ margin:0; background:var(--bg); color:var(--fg);
        font:14px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif; }
  header{ display:flex; align-items:center; gap:14px; padding:14px 20px;
          border-bottom:1px solid var(--line); position:sticky; top:0; background:var(--bg); z-index:5; }
  .brand{ font-weight:700; font-size:20px; letter-spacing:.5px; }
  .meta{ color:var(--dim); font-family:var(--mono); font-size:12px; }
  .spacer{ flex:1; }
  button{ background:var(--panel); color:var(--fg); border:1px solid var(--line);
          border-radius:8px; padding:6px 12px; cursor:pointer; font:inherit; }
  button:hover{ border-color:var(--accent); }
  button.primary{ background:var(--accent); border-color:var(--accent); color:#fff; }
  .dot{ width:8px; height:8px; border-radius:50%; background:var(--dim); display:inline-block; }
  .dot.ok{ background:var(--ok); } .dot.err{ background:var(--err); }
  main{ max-width:1080px; margin:0 auto; padding:22px 20px 60px; }
  .gate{ background:var(--panel); border:1px solid var(--line); border-radius:12px; padding:18px; margin-bottom:22px; }
  .gate input{ background:var(--bg); color:var(--fg); border:1px solid var(--line); border-radius:8px;
               padding:8px 10px; width:min(420px,100%); font-family:var(--mono); }
  .hint{ color:var(--dim); font-size:12px; margin:8px 0 0; }
  .hint code{ font-family:var(--mono); }
  .cards{ display:grid; grid-template-columns:repeat(auto-fit,minmax(120px,1fr)); gap:12px; margin-bottom:22px; }
  .card{ background:var(--panel); border:1px solid var(--line); border-radius:12px; padding:16px; }
  .card .num{ font-size:28px; font-weight:700; }
  .card .lbl{ color:var(--dim); text-transform:uppercase; font-size:11px; letter-spacing:.6px; margin-top:4px; }
  .panel{ background:var(--panel); border:1px solid var(--line); border-radius:12px; padding:16px 18px; margin-bottom:18px; }
  .panel h2{ margin:0 0 12px; font-size:13px; text-transform:uppercase; letter-spacing:.6px; color:var(--dim); }
  .node{ display:flex; align-items:center; gap:8px; padding:4px 0; }
  .node code, code{ font-family:var(--mono); font-size:12px; }
  table{ width:100%; border-collapse:collapse; }
  th,td{ text-align:left; padding:7px 8px; border-bottom:1px solid var(--line); vertical-align:top; }
  th{ color:var(--dim); font-weight:600; font-size:11px; text-transform:uppercase; letter-spacing:.5px; }
  td .desc{ color:var(--dim); }
  .m{ font-family:var(--mono); font-size:11px; font-weight:700; padding:2px 6px; border-radius:5px; }
  .m-GET{ color:var(--ok); border:1px solid var(--ok); } .m-POST{ color:var(--warn); border:1px solid var(--warn); }
  .run{ padding:3px 10px; font-size:12px; }
  .drawer{ position:sticky; bottom:0; }
  .drawer-head{ display:flex; align-items:center; justify-content:space-between; margin-bottom:8px; }
  pre{ background:var(--bg); border:1px solid var(--line); border-radius:8px; padding:12px;
       overflow:auto; max-height:340px; font-family:var(--mono); font-size:12px; margin:0; }
  footer{ text-align:center; color:var(--dim); font-size:12px; padding:20px; }
  footer a{ color:var(--dim); }
  .hidden{ display:none; }
</style>
</head>
<body>
<header>
  <div class="brand">zetta</div>
  <span class="dot" id="status"></span>
  <div class="meta" id="meta">connecting...</div>
  <div class="spacer"></div>
  <button id="refreshBtn">Refresh</button>
  <button id="tokenBtn">Token</button>
</header>
<main>
  <section class="gate hidden" id="gate">
    <div><strong>Bearer token required</strong> to read this node's data.</div>
    <p><input id="tokenInput" type="password" placeholder="paste bearer token" autocomplete="off"> <button class="primary" id="tokenSave">Save</button></p>
    <p class="hint">Get it from your operator: <code>kubectl get secret &lt;cluster&gt;-zetta-mcp -n &lt;ns&gt; -o jsonpath='{.data.token}' | base64 -d</code> &middot; connect over MCP: <a href="/mcp.md">/mcp.md</a></p>
  </section>
  <section class="cards" id="cards"></section>
  <section class="panel"><h2>Cluster</h2><div id="cluster">...</div></section>
  <section class="panel"><h2>Capabilities</h2>
    <table id="caps"><thead><tr><th>Method</th><th>Path</th><th>Summary</th><th></th></tr></thead><tbody></tbody></table>
  </section>
  <section class="panel drawer hidden" id="drawer">
    <div class="drawer-head"><code id="drawerTitle"></code> <button id="drawerClose">close</button></div>
    <pre id="drawerBody"></pre>
  </section>
</main>
<footer>Zetta <span id="ver"></span> &middot; <a href="/mcp.md">MCP</a> &middot; <a href="/v1/openapi.json">OpenAPI</a> &middot; <a href="/llms.txt">llms.txt</a></footer>
<script>
const TK='zetta_token';
let token = sessionStorage.getItem(TK) || '';
const $ = s => document.querySelector(s);
function gate(v){ $('#gate').classList.toggle('hidden',!v); }
function setStatus(ok,msg){ $('#status').className='dot '+(ok?'ok':'err'); $('#meta').textContent=msg; }
async function api(path){
  const h={}; if(token) h['Authorization']='Bearer '+token;
  const r = await fetch(path,{headers:h,cache:'no-store'});
  if(r.status===401){ gate(true); throw new Error('401 - token required'); }
  if(!r.ok) throw new Error(path+' -> '+r.status);
  return r.json();
}
async function loadInfo(){
  try{
    const d = await api('/v1/server_info'); const v = d.value||d; gate(false);
    const nodes = (v.cluster_members||[]);
    setStatus(true, (String(v.node_id||'').slice(0,8))+' &middot; '+nodes.length+' node(s)');
    $('#meta').innerHTML = '<code>'+String(v.node_id||'').slice(0,8)+'</code> &middot; '+nodes.length+' node(s)';
    $('#ver').textContent = v.version||'';
    const cards=[['databases',v.databases],['projects',v.projects],['models',v.models],['jobs',v.jobs],['runs',v.runs],['nodes',nodes.length]];
    $('#cards').innerHTML = cards.map(([k,val])=>'<div class="card"><div class="num">'+(val??'-')+'</div><div class="lbl">'+k+'</div></div>').join('');
    $('#cluster').innerHTML = nodes.length ? nodes.map(m=>'<div class="node"><span class="dot ok"></span><code>'+m+'</code></div>').join('') : '-';
  }catch(e){ setStatus(false, e.message); }
}
async function loadCaps(){
  try{
    const doc = await api('/v1/openapi.json'); const paths=doc.paths||{}; const rows=[];
    for(const p of Object.keys(paths).sort()){
      for(const m of Object.keys(paths[p])){
        const op=paths[p][m]||{}; const M=m.toUpperCase();
        const sum=(op.summary||op.description||'').replace(/</g,'&lt;').slice(0,140);
        const run = M==='GET' ? '<button class="run" data-p="'+p+'">Run</button>' : '';
        rows.push('<tr><td><span class="m m-'+M+'">'+M+'</span></td><td><code>'+p+'</code></td><td class="desc">'+sum+'</td><td>'+run+'</td></tr>');
      }
    }
    $('#caps tbody').innerHTML = rows.join('') || '<tr><td colspan="4" class="desc">no capabilities</td></tr>';
    document.querySelectorAll('.run').forEach(b=> b.onclick=()=>run(b.dataset.p));
  }catch(e){ /* gate/status already surfaced by loadInfo */ }
}
async function run(path){
  $('#drawer').classList.remove('hidden');
  $('#drawerTitle').textContent='GET '+path; $('#drawerBody').textContent='...';
  try{ const d=await api(path); $('#drawerBody').textContent=JSON.stringify(d,null,2); }
  catch(e){ $('#drawerBody').textContent=String(e.message); }
}
function refresh(){ loadInfo(); loadCaps(); }
$('#tokenSave').onclick=()=>{ token=$('#tokenInput').value.trim(); sessionStorage.setItem(TK,token); gate(false); refresh(); };
$('#tokenBtn').onclick=()=>{ $('#tokenInput').value=token; gate(true); };
$('#drawerClose').onclick=()=>$('#drawer').classList.add('hidden');
$('#refreshBtn').onclick=refresh;
refresh();
setInterval(loadInfo, 5000);
</script>
</body>
</html>
)HTML";

}  // namespace

std::string_view web_ui_page() { return kPage; }

}  // namespace zetta::api
