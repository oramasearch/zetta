// The ONE Beast2 adapter TU (docs/api/registry-design.md → Placement + build;
// docs/research/beast2-http.md). No beast2 / cppalliance-http / Boost.JSON
// name may appear outside this file; Boost.JSON <-> nlohmann conversion
// happens exactly once, here, at the wire boundary.
#include "zetta/api/server.hpp"

#include <atomic>
#include <cctype>
#include <thread>
#include <utility>

#include <boost/beast2/http_server.hpp>
#include <boost/capy/io/push_to.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/http/field.hpp>
#include <boost/http/json/json_sink.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/server/cors.hpp>
#include <boost/http/server/router.hpp>
#include <boost/json.hpp>

#include "zetta/api/background.hpp"
#include "zetta/api/bridge.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/llms.hpp"
#include "zetta/api/mcp_docs.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/api/web_ui.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/query/service.hpp"
#include "zetta/repl/governed_console.hpp"
#include "zetta/version.hpp"

namespace zetta::api {

namespace {

constexpr std::string_view kProtocolVersion = "2025-06-18";

// Boost.JSON -> nlohmann, via serialization (the one sanctioned crossing).
nlohmann::json to_nlohmann(const boost::json::value& value) {
    return nlohmann::json::parse(boost::json::serialize(value),
                                 /*cb=*/nullptr, /*allow_exceptions=*/false);
}

std::string jsonrpc_error(const nlohmann::json& id, int code,
                          std::string_view message) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id.is_null() ? nlohmann::json() : id;
    response["error"] = {{"code", code}, {"message", message}};
    return response.dump();
}

std::string jsonrpc_result(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return response.dump();
}

// The beast2 worker reuses ONE response object across requests (and across
// connections) without clearing its headers, and route_params::send only
// fills ETag / Content-Length when they are absent — so the first response's
// values would survive onto every later one, truncating any longer body on
// the wire (#700). Until the vendored worker loop clears the response, stamp
// both explicitly before every send.
void stamp_response_headers(boost::http::route_params& route,
                            std::string_view body) {
    route.res.erase(boost::http::field::etag);
    route.res.erase(boost::http::field::content_length);
    route.res.set_payload_size(body.size());
}

// The bearer presented on a request, or empty ("Bearer <token>" only).
std::string bearer_of(const boost::http::route_params& route) {
    if (route.req.count(boost::http::field::authorization) == 0) return {};
    std::string_view header_value =
        route.req.at(boost::http::field::authorization);
    if (!header_value.starts_with("Bearer ")) return {};
    return std::string(header_value.substr(7));
}

// The value of the named cookie on a request, or empty — parses the Cookie header
// ("name=value; name2=value2"). The server-rendered web UI authenticates from an
// HttpOnly `zetta_session` cookie (its value is a bearer token, validated identically).
std::string cookie_value(const boost::http::route_params& route, std::string_view name) {
    if (route.req.count(boost::http::field::cookie) == 0) return {};
    std::string_view header = route.req.at(boost::http::field::cookie);
    std::size_t pos = 0;
    while (pos < header.size()) {
        const std::size_t semi = header.find(';', pos);
        std::string_view pair = header.substr(
            pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        while (!pair.empty() && pair.front() == ' ') pair.remove_prefix(1);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name)
            return std::string(pair.substr(eq + 1));
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
    return {};
}

// Percent-decode an application/x-www-form-urlencoded token ('+' → space, %XX → byte).
std::string form_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += in[i];
            }
        } else {
            out += in[i];
        }
    }
    return out;
}

// Every field of an x-www-form-urlencoded body, decoded.
std::vector<std::pair<std::string, std::string>> parse_form(std::string_view body) {
    std::vector<std::pair<std::string, std::string>> fields;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t amp = body.find('&', pos);
        std::string_view pair = body.substr(
            pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos)
            fields.emplace_back(form_decode(pair.substr(0, eq)), form_decode(pair.substr(eq + 1)));
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return fields;
}

std::string form_field(const std::vector<std::pair<std::string, std::string>>& fields,
                       std::string_view name) {
    for (const auto& [key, value] : fields)
        if (key == name) return value;
    return {};
}

// Build a capability input document from a submitted web-UI form (#827).
//
// The dialog renders a NESTED object property as a group of `parent.child`
// controls, so decoding cannot be a flat pass any more: scalar properties are
// coerced by the schema exactly as the REST query-string path does
// (`rest_input_from_query`), and an object property is reassembled from its
// `parent.child` fields and coerced against ITS sub-schema. Empty fields are
// dropped so an untouched optional control does not become `""` and trip the
// schema; an object whose every field was left blank is omitted entirely, which
// is what lets create_dataset_from_query's either/or (a saved `connection` name
// OR an inline `connector`) work from one form.
nlohmann::json web_form_input(
    const zetta::api::Capability& capability,
    const std::vector<std::pair<std::string, std::string>>& fields) {
    if (!capability.input_schema.is_object() ||
        !capability.input_schema.contains("properties")) {
        return nlohmann::json::object();
    }
    const nlohmann::json& properties = capability.input_schema["properties"];

    std::vector<std::pair<std::string, std::string>> scalars;
    nlohmann::json input = nlohmann::json::object();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        const nlohmann::json& property = it.value();
        if (property.value("type", "string") == "object" &&
            property.contains("properties")) {
            std::vector<std::pair<std::string, std::string>> nested;
            for (auto sub = property["properties"].begin();
                 sub != property["properties"].end(); ++sub) {
                const std::string value =
                    form_field(fields, it.key() + "." + sub.key());
                if (!value.empty()) nested.emplace_back(sub.key(), value);
            }
            if (!nested.empty()) {
                input[it.key()] =
                    zetta::api::rest_input_from_query(property, nested);
            }
            continue;
        }
        const std::string value = form_field(fields, it.key());
        if (!value.empty()) scalars.emplace_back(it.key(), value);
    }
    for (auto& [key, value] : zetta::api::rest_input_from_query(capability, scalars)
                                  .items()) {
        input[key] = value;
    }
    return input;
}

// A query-string parameter, or `fallback` when absent.
std::string query_param(const boost::http::route_params& route, std::string_view name,
                        std::string_view fallback) {
    for (const auto& parameter : route.url.params())
        if (parameter.key == name) return std::string(parameter.value);
    return std::string(fallback);
}

// A task-oriented button label for a capability (the API verb is not what a
// user should read on a button). Falls back to the de-underscored, capitalized
// name for anything unmapped.
std::string humanize_action(const std::string& name) {
    if (name == "add_dataset") return "Load dataset";
    if (name == "create_dataset_from_query") return "Load from a source";
    if (name == "create_connection") return "Save a connection";
    if (name == "delete_connection") return "Delete connection";
    if (name == "create_index") return "Create index";
    if (name == "expose_index") return "Expose index";
    if (name == "unexpose_index") return "Un-expose index";
    if (name == "expose_model") return "Expose model";
    if (name == "unexpose_model") return "Un-expose model";
    if (name == "create_project") return "New project";
    if (name == "capture_intent") return "Capture intent";
    if (name == "import_n8n") return "Import n8n";
    if (name == "compile_intent") return "Compile intent";
    if (name == "compile_n8n") return "Compile n8n";
    if (name == "compile_python") return "Compile Python";
    if (name == "add_nodes") return "Add nodes";
    if (name == "detect_provisioners") return "Detect providers";
    if (name == "cloud_inspect") return "Inspect cloud";
    if (name == "approve_consent") return "Approve consent";
    if (name == "deny_consent") return "Deny consent";
    if (name == "submit_feedback") return "Send feedback";
    std::string out = name;
    for (char& character : out)
        if (character == '_') character = ' ';
    if (!out.empty())
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// The client-side visual graph editor: reads the embedded graphs, renders an
// interactive SVG canvas (drag steps, drag a step's right port onto another to
// connect, click a link to remove it, x to delete a step, Add step, Save), and
// saves via POST /graph-save. Vanilla JS, no external deps (CSP-safe inline).
constexpr const char* kGraphEditorJs = R"JS(
(function(){
  var raw=document.getElementById('ge-data'); if(!raw) return;
  var DATA=JSON.parse(raw.textContent||'{}');
  var KINDS=DATA.kinds||['connect','clean','model','index','serve','act'];
  var sel=document.getElementById('ge-graph'),canvas=document.getElementById('ge-canvas');
  var saveBtn=document.getElementById('ge-save'),addBtn=document.getElementById('ge-add');
  var NS='http://www.w3.org/2000/svg',NW=158,NH=54,XG=216,YG=94;
  var G=null,svg=null,drag=null,link=null,uid=0,curIdx=0;
  function toast(msg,ok){var t=document.getElementById('ge-toast');if(!t)return;t.textContent=msg;t.className='getoast show'+(ok===false?' err':'');clearTimeout(t._to);t._to=setTimeout(function(){t.className='getoast';},2400);}
  function byId(id){for(var i=0;i<G.nodes.length;i++)if(G.nodes[i].id===id)return G.nodes[i];return null;}
  function el(t,a){var e=document.createElementNS(NS,t);for(var k in a)e.setAttribute(k,a[k]);return e;}
  function layout(){
    var idx={};G.nodes.forEach(function(n,i){idx[n.id]=i;});
    var adj=G.nodes.map(function(){return[];}),ind=G.nodes.map(function(){return 0;});
    G.edges.forEach(function(e){if(idx[e.from]!=null&&idx[e.to]!=null){adj[idx[e.from]].push(idx[e.to]);ind[idx[e.to]]++;}});
    var lay=G.nodes.map(function(){return 0;}),deg=ind.slice(),q=[];
    G.nodes.forEach(function(n,i){if(deg[i]===0)q.push(i);});
    while(q.length){var u=q.shift();adj[u].forEach(function(v){lay[v]=Math.max(lay[v],lay[u]+1);if(--deg[v]===0)q.push(v);});}
    var byL={};G.nodes.forEach(function(n,i){(byL[lay[i]]=byL[lay[i]]||[]).push(i);});
    Object.keys(byL).forEach(function(l){byL[l].forEach(function(ni,k){G.nodes[ni].x=30+(+l)*XG;G.nodes[ni].y=30+k*YG;});});
  }
  function cursor(ev){var r=svg.getBoundingClientRect();return{x:ev.clientX-r.left,y:ev.clientY-r.top};}
  function path(a,b){var x1=a.x+NW,y1=a.y+NH/2,x2=b.x,y2=b.y+NH/2,mx=(x1+x2)/2;return 'M'+x1+' '+y1+' C'+mx+' '+y1+' '+mx+' '+y2+' '+x2+' '+y2;}
  function render(){
    canvas.innerHTML='';
    var W=Math.max(canvas.clientWidth||600,600),H=380;
    G.nodes.forEach(function(n){W=Math.max(W,n.x+NW+40);H=Math.max(H,n.y+NH+40);});
    svg=el('svg',{'class':'gesvg',width:W,height:H});
    var defs=el('defs',{});defs.innerHTML='<marker id="gearw" markerWidth="9" markerHeight="9" refX="8" refY="4.5" orient="auto"><path d="M0 0 9 4.5 0 9z" fill="#9aa1ad"/></marker>';svg.appendChild(defs);
    G.edges.forEach(function(e){var a=byId(e.from),b=byId(e.to);if(!a||!b)return;
      var p=el('path',{d:path(a,b),'class':'geedge'+(e.kind==='control'?' ctrl':''),'marker-end':'url(#gearw)'});
      p.addEventListener('click',function(ev){ev.stopPropagation();G.edges=G.edges.filter(function(x){return!(x.from===e.from&&x.to===e.to);});render();});
      svg.appendChild(p);});
    if(link)svg.appendChild(el('path',{d:'M'+link.x1+' '+link.y1+' L'+link.x2+' '+link.y2,'class':'geedge geghost'}));
    G.nodes.forEach(function(n){
      var g=el('g',{transform:'translate('+n.x+','+n.y+')','class':'genode gk-'+n.kind});
      g.appendChild(el('rect',{width:NW,height:NH,rx:11,'class':'generect'}));
      var kt=el('text',{x:15,y:22,'class':'genkind'});kt.textContent=n.kind;g.appendChild(kt);
      var nt=el('text',{x:15,y:40,'class':'genname'});nt.textContent=(n.name||'').slice(0,20);g.appendChild(nt);
      var port=el('circle',{cx:NW,cy:NH/2,r:6,'class':'geport'});g.appendChild(port);
      var del=el('text',{x:NW-13,y:20,'class':'gedel'});del.textContent='×';g.appendChild(del);
      del.addEventListener('mousedown',function(ev){ev.stopPropagation();});
      del.addEventListener('click',function(ev){ev.stopPropagation();G.nodes=G.nodes.filter(function(x){return x.id!==n.id;});G.edges=G.edges.filter(function(x){return x.from!==n.id&&x.to!==n.id;});render();});
      g.addEventListener('mousedown',function(ev){if(ev.target===port||ev.target===del)return;var p=cursor(ev);drag={n:n,ox:p.x-n.x,oy:p.y-n.y};ev.preventDefault();});
      g.addEventListener('dblclick',function(ev){ev.stopPropagation();drag=null;
        var inp=document.createElement('input');inp.className='genameedit';inp.value=n.name||'';
        inp.style.left=(n.x+13)+'px';inp.style.top=(n.y+27)+'px';canvas.appendChild(inp);inp.focus();inp.select();
        var done=false;function commit(){if(done)return;done=true;n.name=inp.value.trim()||n.name;if(inp.parentNode)inp.parentNode.removeChild(inp);render();}
        inp.addEventListener('blur',commit);
        inp.addEventListener('keydown',function(e){if(e.key==='Enter')commit();else if(e.key==='Escape'){done=true;if(inp.parentNode)inp.parentNode.removeChild(inp);}});});
      port.addEventListener('mousedown',function(ev){ev.stopPropagation();ev.preventDefault();var p=cursor(ev);link={from:n.id,x1:n.x+NW,y1:n.y+NH/2,x2:p.x,y2:p.y};});
      svg.appendChild(g);});
    canvas.appendChild(svg);
  }
  document.addEventListener('mousemove',function(ev){if(!svg)return;
    if(drag){var p=cursor(ev);drag.n.x=Math.max(0,p.x-drag.ox);drag.n.y=Math.max(0,p.y-drag.oy);render();}
    else if(link){var p=cursor(ev);link.x2=p.x;link.y2=p.y;render();}});
  document.addEventListener('mouseup',function(ev){
    if(link){var p=cursor(ev),t=null;G.nodes.forEach(function(n){if(p.x>=n.x&&p.x<=n.x+NW&&p.y>=n.y&&p.y<=n.y+NH)t=n;});
      if(t&&t.id!==link.from&&!G.edges.some(function(e){return e.from===link.from&&e.to===t.id;}))G.edges.push({from:link.from,to:t.id,kind:'data_flow'});
      link=null;render();}
    drag=null;});
  function loadIndex(i){var d=DATA.graphs[i];if(!d)return;curIdx=i;
    G={project:d.project,name:d.name,nodes:(d.nodes||[]).map(function(n){
        var at=(n.attributes&&typeof n.attributes==='object')?n.attributes:{};var ui=at.ui||{};
        var has=(typeof ui.x==='number'&&typeof ui.y==='number');
        return{id:n.id,kind:n.kind,name:n.name,attributes:at,x:has?ui.x:0,y:has?ui.y:0,placed:has};}),
      edges:(d.edges||[]).map(function(e){return{from:e.from,to:e.to,kind:e.kind||'data_flow'};})};
    uid=0;G.nodes.forEach(function(n){var m=(''+n.id).match(/(\d+)$/);if(m)uid=Math.max(uid,+m[1]);});
    if(!G.nodes.some(function(n){return n.placed;}))layout();
    else{var maxY=0;G.nodes.forEach(function(n){if(n.placed)maxY=Math.max(maxY,n.y);});var k=1;
      G.nodes.forEach(function(n){if(!n.placed){n.x=30;n.y=maxY+YG*k;k++;n.placed=true;}});}
    render();}
  var kindSel=document.getElementById('ge-kind');
  if(addBtn)addBtn.addEventListener('click',function(){
    var kind=(kindSel&&kindSel.value)||KINDS[0];uid++;
    var maxY=0;G.nodes.forEach(function(n){maxY=Math.max(maxY,n.y);});
    G.nodes.push({id:'n'+uid,kind:kind,name:kind,attributes:{},x:30,y:(G.nodes.length?maxY+YG:30),placed:true});
    render();});
  if(saveBtn)saveBtn.addEventListener('click',function(){
    var payload={project:G.project,graph:G.name,
      nodes:G.nodes.map(function(n){var a={};for(var k in n.attributes)a[k]=n.attributes[k];a.ui={x:Math.round(n.x),y:Math.round(n.y)};return{id:n.id,kind:n.kind,name:n.name,attributes:a};}),
      edges:G.edges.map(function(e){return{from:e.from,to:e.to,kind:e.kind};})};
    var lbl=saveBtn.querySelector('.gelbl');if(lbl)lbl.textContent='Saving…';
    fetch('/graph-save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
      .then(function(r){return r.json();}).then(function(j){if(lbl)lbl.textContent='Save';
        if(j&&j.ok===false){toast('Save failed: '+(j.error||''),false);}
        else{if(DATA.graphs[curIdx]){DATA.graphs[curIdx].nodes=payload.nodes;DATA.graphs[curIdx].edges=payload.edges;}toast('Graph saved',true);}})
      .catch(function(){if(lbl)lbl.textContent='Save';toast('Save failed',false);});});
  if(sel)sel.addEventListener('change',function(){loadIndex(+sel.value);});
  window.addEventListener('resize',function(){if(G)render();});
  loadIndex(0);
})();
)JS";

// Resolve a search hit's row id to its document json for display — a table's
// store holds its rows even when partitioned. Presentation-only lookup on the
// owning loop; empty object when the row can't be found.
nlohmann::json row_document(zetta::Node& node, const std::string& table,
                            const std::string& row_id) {
    for (const std::string& database_name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(database_name);
        if (!database) continue;
        const std::shared_ptr<zetta::data::Table> found = database->find_table(table);
        if (!found) continue;
        const zetta::storage::StorageEngine& store = found->store();
        for (std::size_t i = 0; i < store.size(); ++i) {
            if (store.id_at(i) != row_id) continue;
            // An unreadable row (a paging engine's failed page read) renders as
            // the empty object here, the same as a row that isn't there — this
            // endpoint has no error channel for a row-level read failure.
            const zetta::storage::ParquetLazyPtr row = store.at(i);
            return row.value_or(nlohmann::json::object());
        }
        return nlohmann::json::object();
    }
    return nlohmann::json::object();
}

// The unified console front-door (#647). One input box → one routing decision, shared by the
// web Shell, Notebooks, and (later) the GUI Console: a leading '/' is a command; otherwise the
// leading keyword auto-detects SQL vs simulated-Python. `/clear` is a pure-UI command handled in
// the browser and never reaches here.
struct ConsoleRoute {
    enum class Kind { Sql, Python, Immediate } kind = Kind::Sql;
    std::string source;          // the statement to run (Sql / Python)
    std::string immediate_json;  // a ready JSON reply (help / unknown command) — no engine run
};

std::string console_help_json() {
    return nlohmann::json{{"messages", nlohmann::json::array({
        "Orama Zetta console \xE2\x80\x94 one box, auto-detected.",
        "  type SQL directly        SELECT * FROM system.tables",
        "  type Python directly     model = rank(customers)",
        "  /sql <statement>         force SQL",
        "  /py <code>               force Python (compiled, gated, audited)",
        "  /tables                  list the system.* tables",
        "  /help                    this message",
        "  /clear                   clear the transcript (browser)",
        "SQL runs through the query engine; Python lowers to the execution graph, passes the",
        "correctness gate, then runs \xE2\x80\x94 every statement is audited."})}}.dump();
}

bool is_sql_leading_keyword(std::string word) {
    for (char& character : word)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    static constexpr std::string_view keywords[] = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP",   "ALTER",
        "WITH",   "SHOW",   "EXPLAIN","TRUNCATE","DESCRIBE","DESC", "PRAGMA",
        "USE",    "VALUES", "GRANT",  "REVOKE", "SET",    "TABLE",  "BEGIN",
        "COMMIT", "ROLLBACK"};
    for (std::string_view keyword : keywords)
        if (word == keyword) return true;
    return false;
}

ConsoleRoute route_console_input(const std::string& raw) {
    ConsoleRoute route;
    const auto begin = raw.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        route.kind = ConsoleRoute::Kind::Immediate;
        route.immediate_json = R"({"messages":[]})";
        return route;
    }
    const auto end = raw.find_last_not_of(" \t\r\n");
    const std::string source = raw.substr(begin, end - begin + 1);

    if (source[0] == '/') {
        const auto space = source.find_first_of(" \t\r\n");
        std::string command =
            source.substr(1, (space == std::string::npos ? source.size() : space) - 1);
        std::string rest;
        if (space != std::string::npos) {
            const auto rest_begin = source.find_first_not_of(" \t\r\n", space);
            if (rest_begin != std::string::npos) rest = source.substr(rest_begin);
        }
        for (char& character : command)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (command == "help") {
            route.kind = ConsoleRoute::Kind::Immediate;
            route.immediate_json = console_help_json();
        } else if (command == "sql") {
            route.kind = ConsoleRoute::Kind::Sql;
            route.source = rest;
        } else if (command == "py" || command == "python") {
            route.kind = ConsoleRoute::Kind::Python;
            route.source = rest;
        } else if (command == "tables") {
            route.kind = ConsoleRoute::Kind::Sql;
            route.source = "SELECT * FROM system.tables";
        } else if (command == "clear") {
            route.kind = ConsoleRoute::Kind::Immediate;
            route.immediate_json = R"({"messages":["clear runs in the browser"]})";
        } else {
            route.kind = ConsoleRoute::Kind::Immediate;
            route.immediate_json =
                nlohmann::json{{"error", "unknown command '/" + command +
                                             "' \xE2\x80\x94 type /help for the list"}}
                    .dump();
        }
        return route;
    }

    // A bare command word (no slash) is a friendly alias — "help" == "/help".
    std::string bare = source;
    for (char& character : bare)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (bare == "help" || bare == "?") {
        route.kind = ConsoleRoute::Kind::Immediate;
        route.immediate_json = console_help_json();
        return route;
    }
    if (bare == "tables") {
        route.kind = ConsoleRoute::Kind::Sql;
        route.source = "SELECT * FROM system.tables";
        return route;
    }
    if (bare == "clear") {
        route.kind = ConsoleRoute::Kind::Immediate;
        route.immediate_json = R"({"messages":["clear runs in the browser"]})";
        return route;
    }

    // No leading slash → auto-detect from the leading token.
    const auto token_end = source.find_first_of(" \t\r\n(");
    const std::string leading =
        source.substr(0, token_end == std::string::npos ? source.size() : token_end);
    route.kind =
        is_sql_leading_keyword(leading) ? ConsoleRoute::Kind::Sql : ConsoleRoute::Kind::Python;
    route.source = source;
    return route;
}

// Whether the request reached us over HTTPS (behind the ingress/Cloudflare) — decides the
// cookie's `Secure` flag (omitted for plain-HTTP local dev, or the login cookie never sticks).
bool request_is_https(const boost::http::route_params& route) {
    return route.req.count("x-forwarded-proto") != 0 &&
           std::string_view(route.req.at("x-forwarded-proto")) == "https";
}

// A capy WriteSink that collects a request body into a std::string — the form-urlencoded bodies the
// server-rendered login + Console POSTs send (json_sink parses JSON, which form bodies are not).
struct StringBodySink {
    std::string data;
    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write_some(const CB& buffers) {
        std::size_t total = 0;
        for (auto it = boost::capy::begin(buffers); it != boost::capy::end(buffers); ++it) {
            const boost::capy::const_buffer buffer(*it);
            data.append(static_cast<const char*>(buffer.data()), buffer.size());
            total += buffer.size();
        }
        return {{{}, total}};
    }
    template <boost::capy::ConstBufferSequence CB>
    boost::capy::immediate<boost::capy::io_result<std::size_t>> write(const CB& buffers) {
        return write_some(buffers);
    }
    boost::capy::immediate<boost::capy::io_result<>> write_eof() { return {{}}; }
};

// The acting identity the ONE auth gate (#745) resolved for a request. Stashed
// on the route's per-request datastore (the worker clears route_data each
// request), so every downstream handler reads a single decision instead of
// re-checking the bearer — the per-handler shared-secret checks are gone. When
// `is_user` the gate resolved the bearer against the token store to a user, so
// `acting_key` is that user's id (authorize against its effective permissions);
// otherwise `acting_key` is the presented bearer (a legacy AccessKey id, or
// empty for the audited unauthenticated-local session).
struct RequestIdentity {
    std::string acting_key;
    bool is_user = false;
};

// The identity the gate stashed for this request, or an empty
// (unauthenticated-local) identity if none was recorded — defensive, since the
// gate is installed as global middleware and runs before every route.
RequestIdentity identity_of(const boost::http::route_params& route) {
    if (const RequestIdentity* identity =
            route.route_data.find<RequestIdentity>()) {
        return *identity;
    }
    return {};
}

// A transport-level REST failure (bad bearer, unreadable body) in the same
// envelope shape as a finished call, minus the trace ids (no call ran).
std::string transport_error_body(std::string_view code,
                                 std::string_view message) {
    return nlohmann::json{
        {"ok", false},
        {"error",
         {{"code", code}, {"message", message}, {"path", ""}}}}.dump();
}

// rest_status's integer mapped onto the wire enum.
boost::http::status http_status_of(int status_code) {
    switch (status_code) {
        case 200: return boost::http::status::ok;
        case 400: return boost::http::status::bad_request;
        case 403: return boost::http::status::forbidden;
        case 404: return boost::http::status::not_found;
        case 422: return boost::http::status::unprocessable_entity;
        case 503: return boost::http::status::service_unavailable;
        default: return boost::http::status::internal_server_error;
    }
}

}  // namespace

struct ApiServer::Impl {
    Node& node;
    const zetta::cluster::Cluster* cluster;
    ServerOptions options;

    Registry capability_registry;
    MainThreadBridge bridge;
    // Owns the worker handles the kind:job capabilities launch (#617); pruned
    // each drain() on the owner thread, where their destructors may join.
    BackgroundActions background;
    // The provision targets this embedding wired (#621) — consulted by the
    // consent-gated add_nodes capability. Empty = no targets wired.
    ProvisionerResolver provisioners;

    boost::corosio::io_context io_context;
    std::unique_ptr<boost::beast2::http_server> server;
    std::thread io_thread;
    std::atomic<bool> is_running{false};
    unsigned short bound_port = 0;
    std::string failure_reason;

    Impl(Node& owning_node, const zetta::cluster::Cluster* owning_cluster,
         ServerOptions server_options)
        : node(owning_node),
          cluster(owning_cluster),
          options(std::move(server_options)) {
        register_core_capabilities(capability_registry);
    }

    // Dispatch one JSON-RPC request document. Returns the response body, or
    // an empty string for notifications (no response required).
    std::string dispatch(const nlohmann::json& request,
                         const std::string& acting_key, bool acting_is_user,
                         const std::string& traceparent_header) {
        if (!request.is_object() || request.value("jsonrpc", "") != "2.0") {
            return jsonrpc_error({}, -32600, "invalid JSON-RPC 2.0 request");
        }
        const nlohmann::json id = request.value("id", nlohmann::json());
        const std::string method = request.value("method", "");

        if (method == "notifications/initialized") return {};
        if (method == "initialize") {
            return jsonrpc_result(
                id, {{"protocolVersion", kProtocolVersion},
                     {"capabilities", {{"tools", nlohmann::json::object()}}},
                     {"serverInfo",
                      {{"name", "zetta"}, {"version", zetta::kVersion}}}});
        }
        if (method == "ping") {
            return jsonrpc_result(id, nlohmann::json::object());
        }
        if (method == "tools/list") {
            nlohmann::json tools = nlohmann::json::array();
            for (const auto& capability : capability_registry.list()) {
                // Per-tool scope (#747): a token-resolved user only sees the tools
                // it may invoke (mcp:call scoped to the tool). Legacy keys + the
                // local session see them all — the same bypass Registry::call uses.
                if (acting_is_user &&
                    !node.may_call_tool(acting_key, /*acting_is_user=*/true,
                                        capability->name)) {
                    continue;
                }
                nlohmann::json tool = {{"name", capability->name},
                                       {"title", capability->title},
                                       {"description", capability->description},
                                       {"inputSchema", capability->input_schema}};
                // The documented success-value shape (#618) — structuredContent
                // carries { ok, value, ... }, and `value` follows this schema.
                if (capability->output_schema.is_object() &&
                    !capability->output_schema.empty()) {
                    tool["outputSchema"] = capability->output_schema;
                }
                tools.push_back(std::move(tool));
            }
            return jsonrpc_result(id, {{"tools", std::move(tools)}});
        }
        if (method == "tools/call") {
            const auto& params = request.value("params", nlohmann::json::object());
            const std::string tool_name = params.value("name", "");
            nlohmann::json arguments =
                params.value("arguments", nlohmann::json::object());
            if (tool_name.empty()) {
                return jsonrpc_error(id, -32602, "params.name is required");
            }

            // The call itself runs on the loop that owns the Node.
            ApiResult call_result;
            auto trace = zetta::observability::parse_traceparent(traceparent_header)
                             .value_or(zetta::observability::make_root_trace());
            // NOTE: this lambda runs inside the owning loop's drain(); the
            // io coroutine is suspended on the bridge until it completes.
            auto work = [&] {
                ApiContext context{node, cluster, acting_key, trace,
                                   &node.span_recorder(), &background,
                                   provisioners};
                context.acting_is_user = acting_is_user;
                call_result =
                    capability_registry.call(tool_name, arguments, context);
            };
            // dispatch() itself is called from the route coroutine via the
            // bridge-aware wrapper below, so here `work` has already been
            // marshaled; see handle_mcp(). This branch never runs off-loop.
            work();

            nlohmann::json payload;
            payload["ok"] = call_result.ok;
            if (call_result.ok) {
                payload["value"] = call_result.value;
            } else {
                payload["error"] = {{"code", call_result.error.code},
                                    {"message", call_result.error.message},
                                    {"path", call_result.error.path}};
                // Structured failure payloads ride along — the consent
                // challenge a "consent_required" refusal names (#621).
                if (!call_result.error.details.is_null()) {
                    payload["error"]["details"] = call_result.error.details;
                }
            }
            payload["trace_id"] = call_result.trace_id;
            payload["span_id"] = call_result.span_id;
            payload["duration_ms"] = call_result.duration_ms;

            nlohmann::json result;
            result["content"] = nlohmann::json::array(
                {{{"type", "text"}, {"text", payload.dump()}}});
            result["structuredContent"] = std::move(payload);
            result["isError"] = !call_result.ok;
            return jsonrpc_result(id, std::move(result));
        }
        return jsonrpc_error(id, -32601, "unknown method '" + method + "'");
    }

    // Serve one llms.txt page (#645): `page` is "index" (/llms.txt), "full"
    // (/llms-full.txt), or a topic name (/llms/<topic>.txt). Auth is enforced
    // at the router-seam gate (#745); rendering touches the Node, so it is
    // marshaled onto the owning loop via the bridge, wrapped in a span (every
    // operation is observable). Shared by the three GET routes in start().
    boost::http::route_task serve_llms(boost::http::route_params& route,
                                       std::string page) {
        namespace http = boost::http;

        // Auth is enforced once, up front, by the router-seam gate (#745) —
        // this route only runs for an already-authorized request.
        std::string traceparent;
        if (route.req.count("traceparent") != 0) {
            traceparent = std::string(route.req.at("traceparent"));
        }
        const auto trace =
            zetta::observability::parse_traceparent(traceparent)
                .value_or(zetta::observability::make_root_trace());

        std::string body;
        bool found = true;
        co_await bridge.run_on_owner([&] {
            zetta::observability::ScopedSpan span(
                &node.span_recorder(), trace, "llms:" + page);
            span.set_attribute("page", page);
            if (page == "index") {
                body = render_llms_index(node, capability_registry);
            } else if (page == "full") {
                body = render_llms_full(node, capability_registry);
            } else {
                auto rendered =
                    render_llms_topic(node, capability_registry, page);
                found = rendered.has_value();
                if (found) body = std::move(*rendered);
            }
            span.finish(found,
                        found ? std::string() : "unknown topic '" + page + "'");
        });

        if (!found) {
            body = "unknown topic '" + page + "'. Topics:";
            for (const auto& topic : llms_topics()) body += " " + topic.name;
            body += "\n";
            route.status(http::status::not_found);
        } else {
            route.status(http::status::ok);
        }
        route.res.set(http::field::content_type, "text/plain; charset=utf-8");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // The serialized OpenAPI document — generated from the registry once at
    // start() (schema-as-code: the same generator output is drift-checked
    // into schemas/openapi.json) and served at GET /v1/openapi.json.
    std::string openapi_body;

    // Serve one REST rendering of a capability (#618, docs/api/rest-openapi.md):
    // GET for reads (input in the query string, coerced by the input schema),
    // POST for actions/jobs (input as the JSON body). Auth is enforced at the
    // router-seam gate (#745), and the call itself — validate, authorize +
    // audit, handle, span —
    // is Registry::call on the owning loop, exactly the MCP path.
    boost::http::route_task serve_rest_capability(
        boost::http::route_params& route,
        std::shared_ptr<const Capability> capability) {
        namespace http = boost::http;

        std::string response_body;
        http::status status = http::status::ok;

        // Auth is enforced once, up front, by the router-seam gate (#745); this
        // route only runs for an already-authorized request, and reads the
        // acting identity the gate resolved.
        const RequestIdentity identity = identity_of(route);

        nlohmann::json input = nlohmann::json::object();
        if (response_body.empty()) {
            if (route.req.method() == http::method::get) {
                std::vector<std::pair<std::string, std::string>> query;
                for (const auto& parameter : route.url.params()) {
                    query.emplace_back(parameter.key, parameter.value);
                }
                input = rest_input_from_query(*capability, query);
            } else {
                http::json_sink body_sink;
                auto [read_error, bytes_read] =
                    co_await boost::capy::push_to(route.req_body, body_sink);
                if (read_error) {
                    status = http::status::bad_request;
                    response_body = transport_error_body(
                        "invalid_input",
                        "could not read the request body as JSON");
                } else if (bytes_read > 0) {
                    input = to_nlohmann(body_sink.release());
                    if (input.is_discarded()) {
                        status = http::status::bad_request;
                        response_body = transport_error_body(
                            "invalid_input",
                            "the request body is not valid JSON");
                    }
                }
            }
        }

        if (response_body.empty()) {
            std::string traceparent;
            if (route.req.count("traceparent") != 0) {
                traceparent = std::string(route.req.at("traceparent"));
            }
            const auto trace =
                zetta::observability::parse_traceparent(traceparent)
                    .value_or(zetta::observability::make_root_trace());
            ApiResult call_result;
            co_await bridge.run_on_owner([&] {
                ApiContext context{node, cluster, identity.acting_key, trace,
                                   &node.span_recorder(), &background};
                context.acting_is_user = identity.is_user;
                call_result =
                    capability_registry.call(capability->name, input, context);
            });
            status = http_status_of(rest_status(call_result));
            response_body = rest_envelope(call_result).dump();
        }

        route.status(status);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, response_body);
        auto [send_error] = co_await route.send(response_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /v1/specs and /v1/specs/<name> (#646, docs/api/specs-index.md): the
    // spec index and each raw document — the OpenAPI document, capability
    // input/output schemas, boundary schemas — served from the C++ accessors,
    // never from disk. Auth is enforced at the router-seam gate (#745);
    // rendering only reads the (start-frozen) registry and static accessors,
    // but is marshaled onto the
    // owning loop so the span lands in the node's ring (every operation is
    // observable). `name` empty = the index.
    boost::http::route_task serve_specs(boost::http::route_params& route,
                                        std::string name) {
        namespace http = boost::http;

        // Auth is enforced once, up front, by the router-seam gate (#745).
        std::string traceparent;
        if (route.req.count("traceparent") != 0) {
            traceparent = std::string(route.req.at("traceparent"));
        }
        const auto trace =
            zetta::observability::parse_traceparent(traceparent)
                .value_or(zetta::observability::make_root_trace());

        std::string body;
        bool found = true;
        co_await bridge.run_on_owner([&] {
            zetta::observability::ScopedSpan span(
                &node.span_recorder(), trace,
                name.empty() ? "specs:index" : "specs:" + name);
            span.set_attribute("spec", name.empty() ? "index" : name);
            if (name.empty()) {
                body = spec_index(capability_registry).dump(2);
            } else {
                const auto spec = find_spec(capability_registry, name);
                found = spec.has_value();
                // The raw document itself — GET-able by codegen as-is; the
                // wrapped { name, kind, schema } shape is get_spec's.
                if (found) body = spec->at("schema").dump(2);
            }
            span.finish(found,
                        found ? std::string() : "unknown spec '" + name + "'");
        });

        if (!found) {
            body = transport_error_body(
                "not_found",
                "unknown spec '" + name + "' — GET /v1/specs for the index");
            route.status(http::status::not_found);
        } else {
            route.status(http::status::ok);
        }
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // Serve one governed entity endpoint (#648, docs/api/serving.md): the
    // exposed-model predict route (POST, JSON body) or the exposed-index
    // search route (GET, query string). Auth is enforced at the router-seam
    // gate (#745); everything else — authorize + audit, the exposure check, the
    // schema
    // boundary, the engine call, the serve:* span — is the HTTP-free serving
    // layer (serving.cpp) on the owning loop, and the response is the same
    // envelope + status mapping as the REST rendering.
    boost::http::route_task serve_entity_endpoint(
        boost::http::route_params& route, bool predict) {
        namespace http = boost::http;

        std::string response_body;
        http::status status = http::status::ok;

        // Auth is enforced once, up front, by the router-seam gate (#745); read
        // the acting identity it resolved for the serving-layer authorize.
        const RequestIdentity identity = identity_of(route);

        std::string target;
        for (const auto& [parameter, value] : route.params) {
            if (parameter == "name") target = value;
        }

        nlohmann::json input = nlohmann::json::object();
        if (response_body.empty()) {
            if (route.req.method() == http::method::get) {
                std::vector<std::pair<std::string, std::string>> query;
                for (const auto& parameter : route.url.params()) {
                    query.emplace_back(parameter.key, parameter.value);
                }
                input = rest_input_from_query(serve_search_request_schema(),
                                              query);
            } else {
                http::json_sink body_sink;
                auto [read_error, bytes_read] =
                    co_await boost::capy::push_to(route.req_body, body_sink);
                if (read_error) {
                    status = http::status::bad_request;
                    response_body = transport_error_body(
                        "invalid_input",
                        "could not read the request body as JSON");
                } else if (bytes_read > 0) {
                    input = to_nlohmann(body_sink.release());
                    if (input.is_discarded()) {
                        status = http::status::bad_request;
                        response_body = transport_error_body(
                            "invalid_input",
                            "the request body is not valid JSON");
                    }
                }
            }
        }

        if (response_body.empty()) {
            std::string traceparent;
            if (route.req.count("traceparent") != 0) {
                traceparent = std::string(route.req.at("traceparent"));
            }
            const auto trace =
                zetta::observability::parse_traceparent(traceparent)
                    .value_or(zetta::observability::make_root_trace());
            ApiResult serve_result;
            co_await bridge.run_on_owner([&] {
                ServeRequest request{target, input, identity.acting_key, trace,
                                     &node.span_recorder()};
                request.acting_is_user = identity.is_user;
                serve_result = predict ? serve_predict(node, request)
                                       : serve_search(node, request);
            });
            status = http_status_of(rest_status(serve_result));
            response_body = rest_envelope(serve_result).dump();
        }

        route.status(status);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, response_body);
        auto [send_error] = co_await route.send(response_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /v1/openapi.json — the OpenAPI rendering. Auth is enforced once, up
    // front, by the router-seam gate (#745).
    boost::http::route_task serve_openapi(boost::http::route_params& route) {
        namespace http = boost::http;
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, openapi_body);
        auto [send_error] = co_await route.send(openapi_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // --- server-rendered web dashboard (#647) --------------------------------
    // Public HTML routes: the browser can't attach a bearer to a plain navigation, so these
    // self-authenticate from an HttpOnly `zetta_session` cookie (its value IS a bearer token,
    // validated identically). Reads render the read model + registry as HTML; the Console POSTs to
    // /run, which invokes a capability through the SAME Registry::call the REST/MCP surfaces use — so
    // the page can never drift, and the engine still authorizes + consent-gates every call. Data is
    // gathered on the owning loop; the HTML is built from the returned JSON.

    // One capability rendered as an action form (from its input_schema) that POSTs to /run.
    // One capability as a collapsible action (native <details>): a compact summary row (name +
    // description) that expands to its input form, laid out in a 2-column field grid.
    // The schema-driven input fields (a 2-column grid) for a capability — shared
    // by the inline action form and the modal action dialog.
    std::string form_option(const std::string& text, const std::string& value, bool selected) {
        return "<option value=\"" + web_ui_escape(value) + "\"" +
               (selected ? " selected" : "") + ">" + web_ui_escape(text) + "</option>";
    }

    // Live-populated <option> lists so a dialog offers a real picker, never a raw
    // id / name text box.
    std::string table_options(ApiContext& context) {
        std::string options = "<option value=\"\">select a table\xE2\x80\xA6</option>";
        for (const std::string& db_name : context.node.database_names()) {
            const auto database = context.node.find_database(db_name);
            if (!database) continue;
            for (const std::string& table : database->table_names())
                options += form_option(table, table, false);
        }
        return options;
    }
    std::string index_options(ApiContext& context) {
        std::string options = "<option value=\"\">select an index\xE2\x80\xA6</option>";
        for (const auto& index : context.node.indexes()) {
            if (!index) continue;
            std::string fields;
            for (std::size_t i = 0; i < index->fields().size(); ++i) {
                if (i) fields += ", ";
                fields += index->fields()[i];
            }
            options += form_option(index->table() + " \xC2\xB7 " + fields, index->id(), false);
        }
        return options;
    }
    std::string model_options(ApiContext& context) {
        std::string options = "<option value=\"\">select a model\xE2\x80\xA6</option>";
        const ApiResult result = capability_registry.call(
            "sql", nlohmann::json{{"statement", "SELECT name FROM system.models"}}, context);
        if (result.ok)
            for (const auto& row : result.value.value("rows", nlohmann::json::array()))
                if (row.is_array() && !row.empty() && row[0].is_string())
                    options += form_option(row[0].get<std::string>(), row[0].get<std::string>(), false);
        return options;
    }
    std::string database_options(ApiContext& context) {
        std::string options = "<option value=\"\">(default)</option>";
        for (const std::string& db_name : context.node.database_names())
            options += form_option(db_name, db_name, false);
        return options;
    }

    // The saved source connections, so the connector dialog offers a picker
    // instead of asking for a credential again (#827). Names only — the store's
    // read surface never carries a token, and neither does this.
    std::string connection_options(ApiContext& context) {
        std::string options =
            "<option value=\"\">\xE2\x80\x94 none: fill in the connector below "
            "\xE2\x80\x94</option>";
        for (const zetta::connect::SavedConnection& saved :
             zetta::connect::list_connections(context.node))
            options += form_option(saved.name + "  (" + saved.config.type + " / " +
                                       saved.config.account + ")",
                                   saved.name, false);
        return options;
    }

    // Whether a schema property holds a credential — rendered as a masked input
    // and never echoed back into the form. Matched on the property name because
    // that is what the schema gives us; the connector's `token` is the case
    // that matters today.
    static bool is_secret_property(const std::string& key) {
        return key == "token" || key == "secret" || key == "password" ||
               key == "api_key";
    }

    // One scalar control. `name` is the form field name (which for a nested
    // object property is `parent.child` — see nested_object_fields).
    std::string form_control(const std::string& name, const std::string& label,
                             const nlohmann::json& property, bool is_required,
                             const std::string& options) {
        const std::string description = property.value("description", "");
        const std::string type = property.value("type", "string");
        const bool full = label == "path" || label == "statement" ||
                          label == "source" || label == "description" ||
                          label == "query" || description.size() > 52;
        std::string out = std::string("<div class=\"") + (full ? "full" : "") +
                          "\"><label>" + web_ui_escape(label) +
                          (is_required ? "<span class=\"req\">*</span>" : "") +
                          "</label>";
        if (!options.empty()) {
            out += "<select name=\"" + web_ui_escape(name) + "\">" + options +
                   "</select>";
        } else {
            const bool secret = is_secret_property(label);
            out += std::string("<input ") + (secret ? "type=\"password\" " : "") +
                   "name=\"" + web_ui_escape(name) + "\" placeholder=\"" +
                   web_ui_escape(description.empty() ? type : description) + "\">";
        }
        out += "</div>";
        return out;
    }

    // A nested object property rendered as its own labelled group of
    // `parent.child` controls (#827). capability_form_fields only ever handled
    // flat scalars, so a capability carrying a structured input — the connector
    // config — had no usable dialog at all. Sub-properties keep the same
    // required-first ordering and the same enum/picker treatment; a credential
    // sub-property is masked.
    std::string nested_object_fields(const std::string& key,
                                     const nlohmann::json& property) {
        if (!property.contains("properties") ||
            !property["properties"].is_object()) {
            return {};
        }
        const nlohmann::json& properties = property["properties"];
        const nlohmann::json required =
            property.value("required", nlohmann::json::array());
        std::vector<std::string> order;
        for (const auto& item : required)
            if (item.is_string() && properties.contains(item.get<std::string>()))
                order.push_back(item.get<std::string>());
        const std::size_t required_count = order.size();
        for (auto it = properties.begin(); it != properties.end(); ++it)
            if (std::find(order.begin(), order.end(), it.key()) == order.end())
                order.push_back(it.key());

        std::string inner;
        for (std::size_t index = 0; index < order.size(); ++index) {
            const std::string& child = order[index];
            const nlohmann::json& sub = properties[child];
            std::string options;
            if (sub.contains("enum"))
                for (const auto& choice : sub["enum"])
                    if (choice.is_string())
                        options += form_option(choice.get<std::string>(),
                                               choice.get<std::string>(),
                                               sub.value("default", std::string()) ==
                                                   choice.get<std::string>());
            inner += form_control(key + "." + child, child, sub,
                                  index < required_count, options);
        }
        return "<div class=\"full\"><fieldset class=\"fgroup\"><legend>" +
               web_ui_escape(key) + "</legend><div class=\"fgrid\">" + inner +
               "</div></fieldset></div>";
    }

    // A section's icon for its action buttons.
    std::string action_icon(const std::string& name) {
        if (name == "add_dataset") return web_ui_icon("dataset");
        if (name == "create_dataset_from_query") return web_ui_icon("dataset");
        if (name == "create_connection" || name == "delete_connection")
            return web_ui_icon("nodes");
        if (name == "create_index") return web_ui_icon("index");
        if (name == "expose_model" || name == "unexpose_model") return web_ui_icon("models");
        if (name == "add_nodes") return web_ui_icon("nodes");
        return web_ui_icon("bolt");
    }

    // The dialog's input fields: a <select> picker where one fits (an enum, or a
    // table / index / model / database / backend reference), else a text input.
    std::string capability_form_fields(const Capability& capability, ApiContext& context) {
        std::string fields;
        const auto& schema = capability.input_schema;
        if (!schema.is_object() || !schema.contains("properties") ||
            !schema["properties"].is_object())
            return fields;
        const nlohmann::json& properties = schema["properties"];
        const nlohmann::json required = schema.value("required", nlohmann::json::array());
        // Required fields first (in declared order), then the rest — the primary
        // inputs lead the form instead of alphabetical property order.
        std::vector<std::string> order;
        auto has_key = [&](const std::string& k) {
            for (const std::string& o : order)
                if (o == k) return true;
            return false;
        };
        for (const auto& item : required)
            if (item.is_string() && properties.contains(item.get<std::string>()))
                order.push_back(item.get<std::string>());
        const std::size_t required_count = order.size();
        for (auto it = properties.begin(); it != properties.end(); ++it)
            if (!has_key(it.key())) order.push_back(it.key());

        for (std::size_t oi = 0; oi < order.size(); ++oi) {
            const std::string& key = order[oi];
            const nlohmann::json& prop = properties[key];
            const bool is_required = oi < required_count;

            // A nested object gets its own group of `parent.child` controls
            // rather than an unusable single text box (#827).
            if (prop.value("type", "string") == "object" &&
                prop.contains("properties")) {
                fields += nested_object_fields(key, prop);
                continue;
            }

            std::string options;
            if (prop.contains("enum")) {
                for (const auto& choice : prop["enum"])
                    if (choice.is_string())
                        options += form_option(choice.get<std::string>(),
                                               choice.get<std::string>(), false);
            } else if (key == "table") {
                options = table_options(context);
            } else if (key == "index") {
                options = index_options(context);
            } else if (key == "model") {
                options = model_options(context);
            } else if (key == "database") {
                options = database_options(context);
            } else if (key == "connection") {
                options = connection_options(context);
            } else if (key == "backend") {
                options = form_option("oramacore_fulltext (BM25F)", "oramacore_fulltext", true) +
                          form_option("memory_search (exact match)", "memory_search", false);
            }
            fields += form_control(key, key, prop, is_required, options);
        }
        return fields;
    }

    // A capability as a modal dialog with real controls. Its form POSTs to /run
    // and, on success, returns to `view` so the page reflects the new state.
    std::string render_action_dialog(const Capability& capability, ApiContext& context,
                                     const std::string& view, const std::string& dialog_id,
                                     const std::string& label) {
        const std::string fields = capability_form_fields(capability, context);
        std::string out =
            "<dialog id=\"" + dialog_id + "\" class=\"dlg\"><form method=\"post\" action=\"/run\">"
            "<input type=\"hidden\" name=\"cap\" value=\"" + web_ui_escape(capability.name) +
            "\"><input type=\"hidden\" name=\"_return\" value=\"/?view=" + web_ui_escape(view) +
            "\"><div class=\"dlghd\"><h3>" + web_ui_escape(label) +
            "</h3><button type=\"button\" class=\"dlgx\" aria-label=\"Close\" "
            "onclick=\"this.closest('dialog').close()\">\xC3\x97</button></div>"
            "<p class=\"dlgsub\">" + web_ui_escape(capability.description) + "</p>";
        if (!fields.empty()) out += "<div class=\"fgrid\">" + fields + "</div>";
        out += "<div class=\"dlgact\"><button type=\"button\" class=\"btn\" "
               "onclick=\"this.closest('dialog').close()\">Cancel</button>"
               "<button class=\"btn btn-primary\">" + web_ui_escape(label) +
               "</button></div></form></dialog>";
        return out;
    }

    // Header action buttons (icon + task label) for a section, each opening its
    // capability dialog. Returns {buttons, dialogs}.
    std::pair<std::string, std::string> section_actions(
        ApiContext& context, const std::string& view,
        std::initializer_list<std::string_view> names) {
        std::string buttons, dialogs;
        for (std::string_view name : names)
            if (auto capability = capability_registry.find(std::string(name))) {
                const std::string dialog_id = "dlg-" + capability->name;
                const std::string label = humanize_action(capability->name);
                buttons += "<button type=\"button\" class=\"btn small\" "
                           "onclick=\"document.getElementById('" + dialog_id + "').showModal()\">" +
                           action_icon(capability->name) + web_ui_escape(label) + "</button>";
                dialogs += render_action_dialog(*capability, context, view, dialog_id, label);
            }
        return {buttons, dialogs};
    }

    // A panel with header action buttons (right-aligned) that open dialogs — the
    // real-application replacement for a bottom stack of forms.
    std::string render_action_panel(ApiContext& context, const std::string& view,
                                    const std::string& icon, const std::string& title,
                                    const std::string& sub,
                                    std::initializer_list<std::string_view> names,
                                    const std::string& body) {
        auto [buttons, dialogs] = section_actions(context, view, names);
        std::string out = "<section class=\"panel\"><div class=\"phead\"><span class=\"ico\">" +
                          web_ui_icon(icon) + "</span><h2>" + web_ui_escape(title) + "</h2>";
        if (!sub.empty()) out += "<span class=\"psub\">" + web_ui_escape(sub) + "</span>";
        if (!buttons.empty()) out += "<div class=\"pactions\">" + buttons + "</div>";
        out += "</div>" + body + dialogs + "</section>";
        return out;
    }

    // A top toolbar of action buttons (where actions aren't tied to one panel).
    std::string render_action_bar(ApiContext& context, const std::string& view,
                                  std::initializer_list<std::string_view> names) {
        auto [buttons, dialogs] = section_actions(context, view, names);
        if (buttons.empty()) return "";
        return "<div class=\"toolbar\">" + buttons + "</div>" + dialogs;
    }

    // A read table with server-side pagination — run the query, slice a 50-row page, add prev/next
    // links. Each table on a view uses a distinct query key so several can page independently.
    std::string render_paged_table(ApiContext& context, const std::string& statement,
                                   const std::string& key, const std::string& view,
                                   const boost::http::route_params& route,
                                   const std::string& extra_qs = "") {
        constexpr int page_size = 50;
        ApiResult result =
            capability_registry.call("sql", nlohmann::json{{"statement", statement}}, context);
        if (!result.ok)
            return "<div class=\"err-box\">" + web_ui_escape(result.error.message) + "</div>";
        const nlohmann::json columns = result.value.value("columns", nlohmann::json::array());
        const nlohmann::json all_rows = result.value.value("rows", nlohmann::json::array());
        const int total = static_cast<int>(all_rows.size());
        int page = std::atoi(query_param(route, key, "0").c_str());
        if (page < 0) page = 0;
        const int start = page * page_size;
        const int end = std::min(start + page_size, total);
        nlohmann::json rows = nlohmann::json::array();
        for (int i = start; i < end; ++i) rows.push_back(all_rows[i]);
        std::string out = web_ui_table(columns, rows);
        if (total > page_size) {
            out += "<div class=\"pager\"><span>showing " +
                   std::to_string(total == 0 ? 0 : start + 1) + "\xE2\x80\x93" + std::to_string(end) +
                   " of " + std::to_string(total) + "</span>";
            const std::string base = "/?view=" + view + "&" + key + "=";
            if (page > 0) out += "<a href=\"" + base + std::to_string(page - 1) + "\">prev</a>";
            if (end < total) out += "<a href=\"" + base + std::to_string(page + 1) + "\">next</a>";
            out += "</div>";
        }
        return out;
    }

    // A plain SQL identifier (letters/digits/underscore). Gates db/table names before they
    // are interpolated into SQL — no injection, and it keeps hostile syntax away from the
    // crash-prone parser (#819).
    static bool safe_ident(const std::string& s) {
        if (s.empty() || s.size() > 128) return false;
        for (char c : s)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
        return true;
    }

    // The Data explorer: databases -> tables -> rows, navigated by ?db= & ?tbl= query params.
    std::string render_data_explorer(ApiContext& context, const boost::http::route_params& route) {
        const std::string sel_db = query_param(route, "db", "");
        const std::string sel_tbl = query_param(route, "tbl", "");
        auto rows_of = [&](const std::string& sql) -> nlohmann::json {
            ApiResult r =
                capability_registry.call("sql", nlohmann::json{{"statement", sql}}, context);
            return r.ok ? r.value.value("rows", nlohmann::json::array()) : nlohmann::json::array();
        };
        auto field = [](const nlohmann::json& row, const char* key) -> std::string {
            if (row.is_object() && row.contains(key) && row.at(key).is_string())
                return row.at(key).get<std::string>();
            return {};
        };

        // ----- databases column (deduped; empty databases aren't in system.datasets yet) -----
        std::vector<std::string> dbs;
        for (const auto& row : rows_of("SELECT database FROM system.datasets")) {
            const std::string d = field(row, "database");
            if (!d.empty() && std::find(dbs.begin(), dbs.end(), d) == dbs.end()) dbs.push_back(d);
        }
        std::sort(dbs.begin(), dbs.end());
        std::string dbcol =
            "<div class=\"expcol\"><div class=\"exphd\">Databases</div><div class=\"expbody\">";
        for (const std::string& d : dbs)
            dbcol += "<a class=\"exprow" + std::string(d == sel_db ? " on" : "") +
                     "\" href=\"/?view=data&db=" + web_ui_escape(d) + "\"><span>" + web_ui_escape(d) +
                     "</span></a>";
        dbcol += "<a class=\"exprow" + std::string(sel_db == "system" ? " on" : "") +
                 "\" href=\"/?view=data&db=system\"><span>system</span><span class=\"ct\">virtual</span></a>"
                 "</div></div>";

        // ----- tables column -----
        std::string tcol =
            "<div class=\"expcol\"><div class=\"exphd\">Tables</div><div class=\"expbody\">";
        if (sel_db.empty()) {
            tcol += "<div class=\"exphint\">Select a database</div>";
        } else if (sel_db == "system") {
            for (const auto& row : rows_of("SELECT table FROM system.tables")) {
                const std::string t = field(row, "table");
                if (t.empty()) continue;
                tcol += "<a class=\"exprow" + std::string(t == sel_tbl ? " on" : "") +
                        "\" href=\"/?view=data&db=system&tbl=" + web_ui_escape(t) + "\"><span>" +
                        web_ui_escape(t) + "</span></a>";
            }
        } else if (!safe_ident(sel_db)) {
            tcol += "<div class=\"exphint\">Invalid database name</div>";
        } else {
            const auto trows =
                rows_of("SELECT dataset, rows FROM system.datasets WHERE database = '" + sel_db + "'");
            if (trows.empty()) tcol += "<div class=\"exphint\">No tables in this database</div>";
            for (const auto& row : trows) {
                const std::string t = field(row, "dataset");
                if (t.empty()) continue;
                std::string count;
                if (row.is_object() && row.contains("rows") && row["rows"].is_number())
                    count = "<span class=\"ct\">" + std::to_string(row["rows"].get<long long>()) +
                            "</span>";
                tcol += "<a class=\"exprow" + std::string(t == sel_tbl ? " on" : "") +
                        "\" href=\"/?view=data&db=" + web_ui_escape(sel_db) + "&tbl=" + web_ui_escape(t) +
                        "\"><span>" + web_ui_escape(t) + "</span>" + count + "</a>";
            }
        }
        tcol += "</div></div>";

        // ----- rows column (fills the remaining width) -----
        std::string rcol =
            "<div class=\"expcol expcol-wide\"><div class=\"exphd\">Rows</div>"
            "<div class=\"expbody expbody-rows\">";
        if (sel_db.empty() || sel_tbl.empty()) {
            rcol += "<div class=\"exphint\">Select a table to view its rows</div>";
        } else if (sel_db == "system" && safe_ident(sel_tbl)) {
            rcol += render_paged_table(context, "SELECT * FROM system." + sel_tbl, "p", "data", route,
                                       "db=system&tbl=" + sel_tbl + "&");
        } else if (safe_ident(sel_db) && safe_ident(sel_tbl)) {
            rcol += render_paged_table(context, "SELECT * FROM " + sel_db + "." + sel_tbl, "p", "data",
                                       route, "db=" + sel_db + "&tbl=" + sel_tbl + "&");
        } else {
            rcol += "<div class=\"exphint\">Invalid table name</div>";
        }
        rcol += "</div></div>";

        std::string out = web_ui_panel("Explorer", "databases \xC2\xB7 tables \xC2\xB7 rows",
                                       "<div class=\"explorer\">" + dbcol + tcol + rcol + "</div>");
        // A Files (large objects / blobs) panel for a selected user database.
        if (!sel_db.empty() && sel_db != "system" && safe_ident(sel_db))
            out += render_files_panel(context, sel_db);
        return out;
    }

    // The Files panel for one database: upload (raw POST /blob), list from system.large_objects,
    // download links, and delete buttons.
    std::string render_files_panel(ApiContext& context, const std::string& db) {
        auto human = [](unsigned long long n) -> std::string {
            if (n < 1024) return std::to_string(n) + " B";
            const char* unit[] = {"KB", "MB", "GB", "TB"};
            double v = static_cast<double>(n) / 1024.0;
            int i = 0;
            while (v >= 1024.0 && i < 3) { v /= 1024.0; ++i; }
            const long long whole = static_cast<long long>(v);
            const long long frac = static_cast<long long>((v - whole) * 10 + 0.5);
            return std::to_string(whole) + "." + std::to_string(frac) + " " + unit[i];
        };
        ApiResult r = capability_registry.call(
            "sql",
            nlohmann::json{{"statement", "SELECT object_id, name, size_bytes, body_present FROM "
                                         "system.large_objects WHERE database = '" + db + "'"}},
            context);
        const nlohmann::json blobs =
            r.ok ? r.value.value("rows", nlohmann::json::array()) : nlohmann::json::array();
        std::string list;
        if (blobs.empty()) {
            list = "<div class=\"emptybox\"><span class=\"eico\">" + web_ui_icon("inbox") +
                   "</span><span class=\"et\">No files yet</span><span class=\"eh\">Upload a file "
                   "above; its bytes sync to the other nodes in the background.</span></div>";
        } else {
            for (const auto& b : blobs) {
                if (!b.is_object()) continue;
                const std::string oid = b.value("object_id", "");
                const std::string name = b.value("name", "");
                std::string size;
                if (b.contains("size_bytes") && b["size_bytes"].is_number())
                    size = human(b["size_bytes"].get<unsigned long long>());
                const bool present = b.value("body_present", true);
                list += "<div class=\"filerow\"><span class=\"fname\">" + web_ui_escape(name) +
                        "</span><span class=\"fsize\">" + size + "</span>" +
                        (present ? "" : "<span class=\"pill warn\">syncing</span>") +
                        "<a class=\"btn small\" href=\"/blob/download?db=" + web_ui_escape(db) +
                        "&id=" + web_ui_escape(oid) +
                        "\">Download</a><button class=\"btn small blobdel\" data-id=\"" +
                        web_ui_escape(oid) + "\" data-name=\"" + web_ui_escape(name) +
                        "\">Delete</button></div>";
            }
        }
        const std::string body =
            "<div class=\"filebar\"><input type=\"file\" id=\"blobfile\">"
            "<button class=\"btn btn-primary\" id=\"blobup\">Upload</button>"
            "<span id=\"blobmsg\" class=\"actionsub\"></span></div><div class=\"filelist\">" +
            list + "</div><script>(function(){var db=" + nlohmann::json(db).dump() +
            ",f=document.getElementById('blobfile'),up=document.getElementById('blobup'),"
            "msg=document.getElementById('blobmsg');"
            "up.onclick=async function(){var file=f.files[0];if(!file){msg.textContent='choose a "
            "file first';return;}msg.textContent='uploading '+file.name+'\\u2026';try{var buf=await "
            "file.arrayBuffer();var r=await fetch('/blob?db='+encodeURIComponent(db)+'&name='+"
            "encodeURIComponent(file.name),{method:'POST',body:buf});var j=await r.json();if(j&&j.ok)"
            "{location.reload();}else{msg.textContent=(j&&j.error)||'upload failed';}}catch(e)"
            "{msg.textContent='error: '+e;}};"
            "document.querySelectorAll('.blobdel').forEach(function(b){b.onclick=async function(){if"
            "(!confirm('Delete '+b.getAttribute('data-name')+'?'))return;try{await "
            "fetch('/blob/delete?db='+encodeURIComponent(db)+'&id='+encodeURIComponent("
            "b.getAttribute('data-id')),{method:'POST'});location.reload();}catch(e){}};});})();"
            "</script>";
        return web_ui_panel("Files", "large objects in " + db, body);
    }

    // A UUID-ish token (hex + hyphens) — gates an engine-issued project id before it goes into a
    // WHERE clause (defense in depth; the id comes from get_project, not raw user input).
    static bool safe_uuid(const std::string& s) {
        if (s.empty() || s.size() > 40) return false;
        for (char c : s)
            if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == '-')) return false;
        return true;
    }

    // Projects — the central object. Index (list) at ?view=projects; a rich detail at
    // ?view=projects&p=<name>: overview + inputs + graphs (from get_project) + related runs/models
    // (system.* filtered by the project uuid) + the lifecycle action forms.
    std::string render_projects(ApiContext& context, const boost::http::route_params& route) {
        const std::string sel = query_param(route, "p", "");

        if (sel.empty()) {
            ApiResult r =
                capability_registry.call("list_projects", nlohmann::json::object(), context);
            std::string list;
            const nlohmann::json names =
                r.ok ? r.value.value("projects", nlohmann::json::array()) : nlohmann::json::array();
            for (const auto& n : names) {
                if (!n.is_string()) continue;
                const std::string name = n.get<std::string>();
                list += "<a class=\"exprow\" href=\"/?view=projects&p=" + web_ui_escape(name) +
                        "\"><span>" + web_ui_escape(name) + "</span></a>";
            }
            if (list.empty())
                list = "<div class=\"emptybox\"><span class=\"eico\">" + web_ui_icon("projects") +
                       "</span><span class=\"et\">No projects yet</span><span class=\"eh\">Create a "
                       "project below, then capture an intent, compile it, gate, deploy, and "
                       "run.</span></div>";
            std::string out = render_action_bar(context, "projects", {"create_project"});
            out += web_ui_panel(
                "Projects",
                "an AI project: intent \xE2\x86\x92 graph \xE2\x86\x92 gate \xE2\x86\x92 deploy "
                "\xE2\x86\x92 run",
                "<div class=\"prjlist\">" + list + "</div>");
            return out;
        }

        // ----- detail -----
        ApiResult r = capability_registry.call("get_project", nlohmann::json{{"name", sel}}, context);
        if (!r.ok)
            return "<div class=\"crumb\"><a href=\"/?view=projects\">Projects</a> / " +
                   web_ui_escape(sel) + "</div><div class=\"err-box\">" +
                   web_ui_escape(r.error.message) + "</div>";
        const nlohmann::json project = r.value;
        const std::string uuid = project.value("id", "");
        const std::string name = project.value("name", sel);
        auto ptable = [&](const std::string& statement, const std::string& key) -> std::string {
            return render_paged_table(context, statement, key, "projects", route, "p=" + sel + "&");
        };

        std::string meta =
            "<div class=\"kv\"><span>id</span><b class=\"mono\">" + web_ui_escape(uuid) +
            "</b></div><div class=\"kv\"><span>owner</span><b>" +
            web_ui_escape(project.value("owner", "\xC2\xB7")) +
            "</b></div><div class=\"kv\"><span>created</span><b class=\"mono\">" +
            web_ui_escape(project.value("created", "")) +
            "</b></div><div class=\"kv\"><span>updated</span><b class=\"mono\">" +
            web_ui_escape(project.value("updated", "")) + "</b></div>";

        std::string out = "<div class=\"crumb\"><a href=\"/?view=projects\">Projects</a> / <b>" +
                          web_ui_escape(name) + "</b></div>";
        out += render_action_bar(context, "projects", {"capture_intent", "import_n8n", "compile_intent",
                                                       "compile_python", "gate", "deploy", "run"});
        out += web_ui_panel(name, "project overview", "<div class=\"kvs\">" + meta + "</div>");
        out += web_ui_panel(
            "Inputs", "captured intents + imported artifacts",
            web_ui_table(nlohmann::json::array(), project.value("inputs", nlohmann::json::array())));
        out += web_ui_panel(
            "Graphs", "compiled execution graphs",
            web_ui_table(nlohmann::json::array(), project.value("graphs", nlohmann::json::array())));
        if (safe_uuid(uuid)) {
            out += web_ui_panel("Runs", "",
                                ptable("SELECT * FROM system.runs WHERE project = '" + uuid + "'", "r"));
            out += web_ui_panel(
                "Models", "",
                ptable("SELECT * FROM system.models WHERE project = '" + uuid + "'", "m"));
        }
        return out;
    }

    // An explanatory legend for the pipeline model — the answer to "what's a run vs a job".
    std::string pipeline_legend() {
        return "<div class=\"legend\"><div class=\"legendhd\"><span class=\"ico\">" +
               web_ui_icon("pipeline") +
               "</span><b>How a pipeline works</b></div>"
               "<div class=\"legendflow\">Project <span>\xE2\x86\x92</span> Intent <span>\xE2\x86\x92"
               "</span> <em>Graph</em> <span>\xE2\x86\x92</span> Gate <span>\xE2\x86\x92</span> Deploy "
               "<span>\xE2\x86\x92</span> <em>Run</em> <span>\xE2\x86\x92</span> <em>Jobs</em> "
               "<span>\xE2\x86\x92</span> Model</div>"
               "<div class=\"legenddefs\">"
               "<div><b>Graph</b> the compiled pipeline (connect \xE2\x86\x92 clean \xE2\x86\x92 model "
               "\xE2\x86\x92 serve) a project's intent lowers to.</div>"
               "<div><b>Run</b> one end-to-end execution of a graph; produces a Model + outputs. One "
               "row per run.</div>"
               "<div><b>Job</b> a unit of native distributed compute a run fans out into (featurize / "
               "train / score) across the cluster \xE2\x80\x94 many jobs per run.</div>"
               "</div></div>";
    }

    // The Graphs panel — aggregates every project's compiled execution graphs (there is no
    // system.graphs view; graphs are owned by projects, reached via get_project).
    // The visual graph editor panel: embeds every project graph's structure (via
    // get_graph) and mounts the interactive SVG editor over it (kGraphEditorJs),
    // which saves through POST /graph-save -> the save_graph capability.
    std::string render_graph_editor(ApiContext& context) {
        nlohmann::json data;
        data["kinds"] = {"connect", "clean", "model", "index", "serve", "act"};
        nlohmann::json graphs = nlohmann::json::array();
        ApiResult projects =
            capability_registry.call("list_projects", nlohmann::json::object(), context);
        if (projects.ok)
            for (const auto& pname : projects.value.value("projects", nlohmann::json::array())) {
                if (!pname.is_string()) continue;
                ApiResult detail = capability_registry.call(
                    "get_project", nlohmann::json{{"name", pname}}, context);
                if (!detail.ok) continue;
                for (const auto& g : detail.value.value("graphs", nlohmann::json::array())) {
                    if (!g.is_object()) continue;
                    ApiResult full = capability_registry.call(
                        "get_graph",
                        nlohmann::json{{"project", pname}, {"graph", g.value("name", "")}},
                        context);
                    if (full.ok) graphs.push_back(full.value);
                }
            }
        data["graphs"] = graphs;

        if (graphs.empty())
            return web_ui_panel(
                "Graph editor",
                "the visual execution graph \xC2\xB7 connect \xE2\x86\x92 clean \xE2\x86\x92 model "
                "\xE2\x86\x92 index \xE2\x86\x92 serve \xE2\x86\x92 act",
                "<div class=\"emptybox\"><span class=\"eico\">" + web_ui_icon("pipeline") +
                    "</span><span class=\"et\">No graphs yet</span><span class=\"eh\">Compile a "
                    "project's intent (Projects \xE2\x86\x92 open one \xE2\x86\x92 compile) to get an "
                    "editable graph here.</span></div>");

        std::string options;
        for (std::size_t i = 0; i < graphs.size(); ++i)
            options += "<option value=\"" + std::to_string(i) + "\">" +
                       web_ui_escape(graphs[i].value("project", "") + " \xC2\xB7 " +
                                     graphs[i].value("name", "")) + "</option>";
        const std::string body =
            "<div class=\"getoolbar\"><select id=\"ge-graph\">" + options +
            "</select><span class=\"gespring\"></span>"
            "<select id=\"ge-kind\" class=\"gekind\"><option value=\"connect\">connect</option>"
            "<option value=\"clean\">clean</option><option value=\"model\">model</option>"
            "<option value=\"index\">index</option><option value=\"serve\">serve</option>"
            "<option value=\"act\">act</option></select>"
            "<button type=\"button\" class=\"btn small\" id=\"ge-add\">" + web_ui_icon("bolt") +
            "Add step</button>"
            "<button type=\"button\" class=\"btn small btn-primary\" id=\"ge-save\">" +
            web_ui_icon("play") + "<span class=\"gelbl\">Save</span></button></div>"
            "<div id=\"ge-canvas\" class=\"gecanvas\"></div>"
            "<div class=\"gehint\">drag a step to move it \xC2\xB7 double-click to rename \xC2\xB7 drag "
            "from a step's right dot onto another to connect \xC2\xB7 click a link to remove it "
            "\xC2\xB7 \xC3\x97 deletes a step</div>"
            "<div id=\"ge-toast\" class=\"getoast\"></div>"
            "<script id=\"ge-data\" type=\"application/json\">" + data.dump() + "</script>"
            "<script>" + std::string(kGraphEditorJs) + "</script>";
        return web_ui_panel("Graph editor",
                            "the visual execution graph \xC2\xB7 connect \xE2\x86\x92 clean "
                            "\xE2\x86\x92 model \xE2\x86\x92 index \xE2\x86\x92 serve \xE2\x86\x92 act",
                            body);
    }

    std::string render_graphs_panel(ApiContext& context) {
        ApiResult lp = capability_registry.call("list_projects", nlohmann::json::object(), context);
        std::string rows;
        if (lp.ok)
            for (const auto& name : lp.value.value("projects", nlohmann::json::array())) {
                if (!name.is_string()) continue;
                const std::string pname = name.get<std::string>();
                ApiResult gp =
                    capability_registry.call("get_project", nlohmann::json{{"name", pname}}, context);
                if (!gp.ok) continue;
                for (const auto& g : gp.value.value("graphs", nlohmann::json::array())) {
                    if (!g.is_object()) continue;
                    const std::string gid = g.value("id", "");
                    rows += "<tr><td>" + web_ui_escape(g.value("name", "")) +
                            "</td><td><a class=\"mono\" href=\"/?view=projects&p=" +
                            web_ui_escape(pname) + "\">" + web_ui_escape(pname) +
                            "</a></td><td class=\"mono\">" + web_ui_escape(gid.substr(0, 8)) +
                            "</td></tr>";
                }
            }
        std::string html =
            rows.empty()
                ? "<div class=\"emptybox\"><span class=\"eico\">" + web_ui_icon("pipeline") +
                      "</span><span class=\"et\">No graphs yet</span><span class=\"eh\">A graph is "
                      "created when you compile a project's intent (Projects \xE2\x86\x92 open a "
                      "project \xE2\x86\x92 compile). Then gate, deploy, and run it.</span></div>"
                : "<div class=\"twrap\"><table><thead><tr><th>Graph</th><th>Project</th><th>id</th>"
                  "</tr></thead><tbody>" + rows + "</tbody></table></div>";
        return web_ui_panel("Graphs",
                            "compiled execution pipelines \xC2\xB7 what a project's intent lowers to",
                            html);
    }

    // A real search experience (not a raw capability form): an index picker + a
    // query box, results rendered as a ranked list of the matching documents with
    // their score and a scatter-gather summary. Reads sq / sq_target from the query
    // string and runs the same `search` capability the MCP/REST surfaces do.
    std::string render_search_panel(ApiContext& context,
                                    const boost::http::route_params& route) {
        std::vector<std::shared_ptr<zetta::index::Index>> search_indexes;
        for (const auto& index : context.node.indexes())
            if (index && index->kind() == zetta::index::IndexKind::Search)
                search_indexes.push_back(index);
        if (search_indexes.empty())
            return web_ui_panel(
                "Search", "full-text search over a table",
                "<div class=\"emptybox\">No search indexes yet \xE2\x80\x94 create one "
                "below to make a table searchable.</div>");

        const std::string query = query_param(route, "sq", "");
        std::string target = query_param(route, "sq_target", "");
        if (target.empty()) target = search_indexes.front()->id();

        auto index_fields = [](const std::shared_ptr<zetta::index::Index>& index) {
            std::string fields;
            for (std::size_t i = 0; i < index->fields().size(); ++i) {
                if (i) fields += ", ";
                fields += index->fields()[i];
            }
            return fields;
        };

        std::string form =
            "<form class=\"searchbar\" method=\"get\" action=\"/\">"
            "<input type=\"hidden\" name=\"view\" value=\"data\">"
            "<select name=\"sq_target\">";
        for (const auto& index : search_indexes)
            form += "<option value=\"" + web_ui_escape(index->id()) + "\"" +
                    (index->id() == target ? " selected" : "") + ">" +
                    web_ui_escape(index->table() + " \xC2\xB7 " + index_fields(index)) +
                    "</option>";
        form += "</select><input name=\"sq\" value=\"" + web_ui_escape(query) +
                "\" placeholder=\"Search documents\xE2\x80\xA6\" autofocus>"
                "<button class=\"btn btn-primary\">Search</button></form>";

        std::string results;
        if (!query.empty()) {
            ApiResult result = capability_registry.call(
                "search", nlohmann::json{{"target", target}, {"query", query}}, context);
            if (!result.ok) {
                results = "<div class=\"err-box\">" + web_ui_escape(result.error.message) +
                          "</div>";
            } else {
                const nlohmann::json& value = result.value;
                const nlohmann::json hits = value.value("hits", nlohmann::json::array());
                const std::string table = value.value("table", "");
                const double ms = value.value("duration_ms", 0.0);
                char timing[32];
                if (ms < 10.0)
                    std::snprintf(timing, sizeof(timing), "%.0f \xC2\xB5s", ms * 1000.0);
                else
                    std::snprintf(timing, sizeof(timing), "%.1f ms", ms);
                const std::size_t node_count =
                    value.contains("nodes") ? value["nodes"].size() : 0;
                results += "<div class=\"metaline\" style=\"margin:16px 0 10px\">" +
                           std::to_string(hits.size()) +
                           (hits.size() == 1 ? " result" : " results") + " \xC2\xB7 " +
                           web_ui_escape(value.value("path", "")) + " \xC2\xB7 " +
                           std::to_string(node_count) +
                           (node_count == 1 ? " node" : " nodes") + " \xC2\xB7 " + timing +
                           "</div>";
                if (hits.empty()) {
                    results += "<div class=\"emptybox\">No documents matched \xE2\x80\x9C" +
                               web_ui_escape(query) + "\xE2\x80\x9D.</div>";
                } else {
                    results += "<div class=\"hits\">";
                    int rank = 0;
                    for (const nlohmann::json& hit : hits) {
                        ++rank;
                        const std::string row_id = hit.value("id", "");
                        char score[16];
                        std::snprintf(score, sizeof(score), "%.3f", hit.value("score", 0.0));
                        results += "<div class=\"hit\"><div class=\"hitmeta\"><span "
                                   "class=\"rank\">#" +
                                   std::to_string(rank) + "</span><span>score " + score +
                                   "</span></div>";
                        const nlohmann::json document =
                            row_document(context.node, table, row_id);
                        if (document.is_object() && !document.empty()) {
                            for (auto it = document.begin(); it != document.end(); ++it) {
                                const std::string text = it.value().is_string()
                                                             ? it.value().get<std::string>()
                                                             : it.value().dump();
                                results += "<div class=\"fld\"><span class=\"lbl\">" +
                                           web_ui_escape(it.key()) + "</span><span>" +
                                           web_ui_escape(text) + "</span></div>";
                            }
                        } else {
                            results += "<div class=\"sub mono\">" + web_ui_escape(row_id) +
                                       "</div>";
                        }
                        results += "</div>";
                    }
                    results += "</div>";
                }
            }
        }
        return web_ui_panel("Search",
                            "full-text search over a table \xC2\xB7 ranked by relevance",
                            form + results);
    }

    std::string render_dashboard_content(ApiContext& context, const std::string& view,
                                         const boost::http::route_params& route) {
        auto ptable = [&](const std::string& statement, const std::string& key) -> std::string {
            return render_paged_table(context, statement, key, view, route);
        };
        std::string out;
        if (view == "overview") {
            ApiResult info =
                capability_registry.call("server_info", nlohmann::json::object(), context);
            if (info.ok)
                out += web_ui_panel(
                    "Overview", "",
                    web_ui_overview_cards(info.value) + "<div class=\"metaline mono\">" +
                        web_ui_escape(info.value.value("node_id", "")) + " \xC2\xB7 v" +
                        web_ui_escape(info.value.value("version", "")) + "</div>");
            out += web_ui_panel("Cluster", "membership \xC2\xB7 reachability \xC2\xB7 sync",
                                ptable("SELECT * FROM system.nodes", "n"));
        } else if (view == "cluster") {
            out += render_action_panel(
                context, view, "cluster", "Nodes",
                "cluster members \xC2\xB7 reachability, liveness, and CRDT replication sync",
                {"add_nodes", "detect_provisioners", "cloud_inspect"},
                ptable("SELECT * FROM system.nodes", "n"));
            out += web_ui_panel("Node metrics", "per-node resource + throughput metrics", ptable("SELECT * FROM system.node_metrics", "nm"));
            out += web_ui_panel("Deployments", "graphs placed onto targets, made runnable", ptable("SELECT * FROM system.deployments", "dep"));
        } else if (view == "projects") {
            out += render_projects(context, route);
        } else if (view == "data") {
            out += render_search_panel(context, route);
            out += render_action_panel(
                context, view, "dataset", "Datasets",
                "tables loaded into the node \xC2\xB7 the source for search + training",
                {"add_dataset", "create_dataset_from_query"},
                ptable("SELECT * FROM system.datasets", "ds"));
            out += render_action_panel(
                context, view, "nodes", "Source connections",
                "saved warehouse credentials, held on every node \xC2\xB7 pick one in "
                "\"Load from a source\" instead of retyping a token",
                {"create_connection", "delete_connection"},
                ptable("SELECT * FROM system.connections", "conn"));
            out += render_action_panel(
                context, view, "index", "Indexes",
                "full-text / vector indexes over a table \xC2\xB7 create one to make a table searchable",
                {"create_index", "expose_index", "unexpose_index"},
                ptable("SELECT * FROM system.indexes", "idx"));
            out += render_data_explorer(context, route);
        } else if (view == "models") {
            out += render_action_panel(
                context, view, "models", "Models",
                "trained models produced by runs \xC2\xB7 weights, metrics + ranked drivers; expose "
                "one to serve predictions",
                {"expose_model", "unexpose_model"},
                ptable("SELECT * FROM system.models", "m"));
        } else if (view == "pipeline") {
            out += render_action_bar(context, view, {"create_project", "capture_intent", "import_n8n",
                                                     "compile_intent", "compile_n8n", "compile_python",
                                                     "gate", "deploy", "run"});
            out += pipeline_legend();
            out += render_graph_editor(context);
            out += web_ui_panel(
                "Deployments",
                "a graph placed onto a target (local / kubernetes / cloud) and made runnable",
                ptable("SELECT * FROM system.deployments", "dep"));
            out += web_ui_panel(
                "Runs", "each row is ONE end-to-end execution of a graph \xC2\xB7 produces a model + outputs",
                ptable("SELECT * FROM system.runs", "runs"));
            out += web_ui_panel(
                "Jobs",
                "the native distributed-compute units a run fans out into (featurize / train / "
                "score) \xC2\xB7 many jobs per run",
                ptable("SELECT * FROM system.jobs", "jobs"));
        } else if (view == "observability") {
            out += web_ui_panel(
                "Traces", "one row per operation (an MCP/REST call, a run) \xC2\xB7 its intent, timing + result",
                ptable("SELECT * FROM system.traces", "tr"));
            out += web_ui_panel("Spans", "the timed steps inside a trace (parent \xE2\x86\x92 child), with durations",
                                ptable("SELECT * FROM system.spans", "sp"));
            out += web_ui_panel("Crashes", "captured crash reports, with stacktraces",
                                ptable("SELECT * FROM system.crashes", "cr"));
        } else if (view == "governance") {
            out += render_action_bar(context, view, {"approve_consent", "deny_consent", "submit_feedback"});
            out += web_ui_panel("Users", "principals that authenticate \xC2\xB7 each holds roles",
                                ptable("SELECT * FROM system.users", "u"));
            out += web_ui_panel("Roles", "named capability bundles assigned to users",
                                ptable("SELECT * FROM system.roles", "r"));
            out += web_ui_panel("Policies", "row / column access rules",
                                ptable("SELECT * FROM system.policies", "pol"));
            out += web_ui_panel("Tokens", "issued bearer access tokens + their scope",
                                ptable("SELECT * FROM system.tokens", "tok"));
            out += web_ui_panel("Consent requests", "pending cloud-action approvals (deny-by-default)",
                                ptable("SELECT * FROM system.consent_requests", "cons"));
            out += web_ui_panel("Audit", "the immutable, append-only record of every governed action",
                                ptable("SELECT * FROM system.audit", "aud"));
        } else if (view == "shell") {
            out += web_ui_shell_repl();
        } else if (view == "notebooks") {
            out += web_ui_notebooks();
        } else {
            out += "<div class=\"empty\">Unknown view.</div>";
        }
        return out;
    }

    // GET / — the dashboard (or the login page when the session cookie is absent/invalid).
    boost::http::route_task serve_web_root(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        std::string body;
        if (user_id.empty()) {
            body = web_ui_login("");
        } else {
            const std::string view = query_param(route, "view", "overview");
            std::string content;
            std::string meta;
            co_await bridge.run_on_owner([&] {
                ApiContext context{node, cluster, user_id,
                                   zetta::observability::make_root_trace(), &node.span_recorder(),
                                   &background};
                context.acting_is_user = true;
                ApiResult info =
                    capability_registry.call("server_info", nlohmann::json::object(), context);
                if (info.ok) {
                    std::string node_id = info.value.value("node_id", "");
                    if (node_id.size() > 12) node_id.resize(12);
                    const std::size_t nodes = info.value.contains("cluster_members")
                                                  ? info.value["cluster_members"].size()
                                                  : 0;
                    meta = node_id + " \xC2\xB7 v" + info.value.value("version", "") + " \xC2\xB7 " +
                           std::to_string(nodes) + " nodes";
                }
                content = render_dashboard_content(context, view, route);
            });
            body = web_ui_shell(view, user_id.size() > 12 ? user_id.substr(0, 12) : user_id, meta,
                                content);
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "text/html; charset=utf-8");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /login — exchange a bearer token for the HttpOnly session cookie.
    boost::http::route_task serve_web_login(boost::http::route_params& route) {
        namespace http = boost::http;
        StringBodySink sink;
        co_await boost::capy::push_to(route.req_body, sink);
        const std::string token = form_field(parse_form(sink.data), "token");
        bool ok = false;
        if (!token.empty())
            co_await bridge.run_on_owner([&] { if (node.authenticate_token(token)) ok = true; });
        if (!ok) {
            const std::string body = web_ui_login("Invalid or expired token.");
            route.status(http::status::unauthorized);
            route.res.set(http::field::content_type, "text/html; charset=utf-8");
            stamp_response_headers(route, body);
            auto [send_error] = co_await route.send(body);
            if (send_error) co_return http::route_error(send_error);
            co_return http::route_done;
        }
        std::string cookie = "zetta_session=" + token + "; HttpOnly; SameSite=Strict; Path=/";
        if (request_is_https(route)) cookie += "; Secure";
        route.status(http::status::found);
        route.res.set(http::field::set_cookie, cookie);
        route.res.set(http::field::location, "/");
        stamp_response_headers(route, "");
        auto [send_error] = co_await route.send("");
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /logout — clear the session cookie.
    boost::http::route_task serve_web_logout(boost::http::route_params& route) {
        namespace http = boost::http;
        route.status(http::status::found);
        route.res.set(http::field::set_cookie,
                      "zetta_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        route.res.set(http::field::location, "/");
        stamp_response_headers(route, "");
        auto [send_error] = co_await route.send("");
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /run — invoke a capability from a Console form (input coerced by its schema).
    boost::http::route_task serve_web_run(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        StringBodySink sink;
        co_await boost::capy::push_to(route.req_body, sink);
        std::string body;
        if (user_id.empty()) {
            body = web_ui_login("");
        } else {
            const auto fields = parse_form(sink.data);
            const std::string cap_name = form_field(fields, "cap");
            const std::string return_url = form_field(fields, "_return");
            std::string content;
            bool succeeded = false;
            co_await bridge.run_on_owner([&] {
                auto capability = capability_registry.find(cap_name);
                if (!capability) {
                    content = web_ui_panel("Unknown capability", "",
                                           "<div class=\"err-box\">" + web_ui_escape(cap_name) +
                                               "</div>");
                    return;
                }
                const nlohmann::json input = web_form_input(*capability, fields);
                ApiContext context{node, cluster, user_id,
                                   zetta::observability::make_root_trace(), &node.span_recorder(),
                                   &background};
                context.acting_is_user = true;
                ApiResult result = capability_registry.call(cap_name, input, context);
                succeeded = result.ok;
                std::string result_html;
                if (result.ok) {
                    result_html = "<div class=\"pill ok\" style=\"margin-bottom:10px\">ok</div><pre>" +
                                  web_ui_escape(result.value.dump(2)) + "</pre>";
                } else {
                    result_html = "<div class=\"err-box\">" + web_ui_escape(result.error.code) +
                                  ": " + web_ui_escape(result.error.message) + "</div>";
                    if (!result.error.details.is_null())
                        result_html += "<pre>" + web_ui_escape(result.error.details.dump(2)) + "</pre>";
                }
                result_html += "<div style=\"margin-top:12px\"><a class=\"btn small\" href=\"/?view=console\">Back to Console</a></div>";
                content = web_ui_panel(cap_name, capability->description, result_html);
            });
            // A dialog-launched mutation returns to the view it came from so the
            // page reflects the new state, instead of a standalone result page
            // (relative same-origin targets only — never an open redirect).
            if (succeeded && return_url.size() > 1 && return_url[0] == '/' &&
                return_url[1] != '/') {
                route.status(http::status::found);
                route.res.set(http::field::location, return_url);
                stamp_response_headers(route, "");
                auto [redirect_error] = co_await route.send("");
                if (redirect_error) co_return http::route_error(redirect_error);
                co_return http::route_done;
            }
            body = web_ui_shell("console",
                                user_id.size() > 12 ? user_id.substr(0, 12) : user_id, "", content);
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "text/html; charset=utf-8");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /shell — run one command (SQL) for the Shell/Notebooks REPL; returns JSON (columns/rows or
    // an error). Cookie-authed and audited as the signed-in user, exactly like the REST sql capability.
    // POST /graph-save — the visual editor's save path: a cookie-authed JSON body
    // {project, graph, nodes, edges} invoked through the save_graph capability.
    boost::http::route_task serve_graph_save(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        StringBodySink sink;
        co_await boost::capy::push_to(route.req_body, sink);
        std::string json_body;
        if (user_id.empty()) {
            json_body = R"({"ok":false,"error":"unauthorized"})";
        } else {
            nlohmann::json input = nlohmann::json::parse(sink.data, nullptr, false);
            if (input.is_discarded()) {
                json_body = R"({"ok":false,"error":"malformed json"})";
            } else {
                co_await bridge.run_on_owner([&] {
                    ApiContext context{node, cluster, user_id,
                                       zetta::observability::make_root_trace(),
                                       &node.span_recorder(), &background};
                    context.acting_is_user = true;
                    ApiResult result =
                        capability_registry.call("save_graph", input, context);
                    json_body = result.ok
                                    ? result.value.dump()
                                    : nlohmann::json{{"ok", false},
                                                     {"error", result.error.message}}
                                          .dump();
                });
            }
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, json_body);
        auto [send_error] = co_await route.send(json_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    boost::http::route_task serve_web_shell(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        StringBodySink sink;
        co_await boost::capy::push_to(route.req_body, sink);
        std::string json_body;
        if (user_id.empty()) {
            json_body = R"({"error":"unauthorized"})";
        } else {
            const ConsoleRoute route_decision =
                route_console_input(form_field(parse_form(sink.data), "q"));
            if (route_decision.kind == ConsoleRoute::Kind::Immediate) {
                // /help, /clear echo, unknown command, or empty — no engine run.
                json_body = route_decision.immediate_json;
            } else if (route_decision.kind == ConsoleRoute::Kind::Sql) {
                // SQL keeps the enforced + audited capability path (per-user column masking).
                co_await bridge.run_on_owner([&] {
                    ApiContext context{node, cluster, user_id,
                                       zetta::observability::make_root_trace(),
                                       &node.span_recorder(), &background};
                    context.acting_is_user = true;
                    ApiResult result = capability_registry.call(
                        "sql", nlohmann::json{{"statement", route_decision.source}}, context);
                    json_body = result.ok ? result.value.dump()
                                          : nlohmann::json{{"error", result.error.message}}.dump();
                });
            } else {
                // Python runs through the governed console (compile → correctness gate → run →
                // audit), gated by an explicit per-user capability check (#745). Same governance
                // chain as the desktop Console, reached over the web.
                co_await bridge.run_on_owner([&] {
                    const zetta::governance::Operation operation =
                        zetta::repl::required_operation(zetta::repl::Frontend::SimPython);
                    const zetta::governance::AccessDecision decision =
                        node.authorize_user(user_id, operation);
                    if (!decision.allowed) {
                        json_body =
                            nlohmann::json{
                                {"error", "blocked: " +
                                              (decision.reason.empty() ? std::string("not permitted")
                                                                       : decision.reason) +
                                              " \xE2\x80\x94 you lack orchestration:start_run"}}
                                .dump();
                        return;
                    }
                    zetta::query::QueryService query_service(node, cluster);
                    zetta::repl::GovernedConsole console(node, query_service);
                    // Enforcement is done above by authorize_user; the console runs unkeyed
                    // (audited as the local web session) but still gates + audits every run.
                    const zetta::repl::Result result =
                        console.run(zetta::repl::Frontend::SimPython, route_decision.source);
                    nlohmann::json body;
                    body["columns"] = result.columns;
                    body["rows"] = result.rows;
                    body["messages"] = result.messages;
                    if (!result.elapsed.empty()) body["elapsed"] = result.elapsed;
                    if (!result.ok()) body["error"] = result.error;
                    json_body = body.dump();
                });
            }
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, json_body);
        auto [send_error] = co_await route.send(json_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /blob?db=<database>&name=<filename> — store the raw request body as a large object
    // (file/blob) in the database's LargeObjectStore. Cookie-authed like the rest of the web UI;
    // the body IS the file bytes (the client reads the File and posts it raw — no multipart parser).
    boost::http::route_task serve_blob_upload(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        const std::string db = query_param(route, "db", "");
        const std::string name = query_param(route, "name", "");
        StringBodySink sink;
        co_await boost::capy::push_to(route.req_body, sink);
        std::string json_body;
        if (user_id.empty()) {
            json_body = R"({"error":"unauthorized"})";
        } else if (db.empty() || name.empty()) {
            json_body = R"({"error":"db and name query params are required"})";
        } else if (sink.data.empty()) {
            json_body = R"({"error":"empty file"})";
        } else {
            co_await bridge.run_on_owner([&] {
                const std::shared_ptr<zetta::data::Database> database = node.find_database(db);
                if (!database) {
                    json_body = nlohmann::json{{"error", "no such database '" + db + "'"}}.dump();
                    return;
                }
                const std::optional<zetta::data::LargeObject> stored =
                    database->large_objects().import_bytes(name, sink.data);
                json_body = stored ? nlohmann::json{{"ok", true},
                                                    {"id", stored->id},
                                                    {"name", stored->name},
                                                    {"size", stored->size}}
                                         .dump()
                                   : R"({"error":"could not store the file"})";
            });
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, json_body);
        auto [send_error] = co_await route.send(json_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /blob/delete?db=<database>&id=<object_id> — unlink a large object.
    boost::http::route_task serve_blob_delete(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        const std::string db = query_param(route, "db", "");
        const std::string id = query_param(route, "id", "");
        StringBodySink sink;
        co_await boost::capy::push_to(route.req_body, sink);
        std::string json_body;
        if (user_id.empty()) {
            json_body = R"({"error":"unauthorized"})";
        } else {
            co_await bridge.run_on_owner([&] {
                const std::shared_ptr<zetta::data::Database> database = node.find_database(db);
                if (!database) {
                    json_body = R"({"error":"no such database"})";
                    return;
                }
                const bool ok = database->large_objects().unlink(id);
                json_body = nlohmann::json{{"ok", ok}}.dump();
            });
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, json_body);
        auto [send_error] = co_await route.send(json_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /blob/download?db=<database>&id=<object_id> — stream a stored blob back as an attachment.
    boost::http::route_task serve_blob_download(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        const std::string db = query_param(route, "db", "");
        const std::string id = query_param(route, "id", "");
        std::string bytes, filename, error;
        if (!user_id.empty())
            co_await bridge.run_on_owner([&] {
                const std::shared_ptr<zetta::data::Database> database = node.find_database(db);
                if (!database) {
                    error = "no such database";
                    return;
                }
                const std::optional<std::string> data = database->large_objects().read(id);
                if (!data) {
                    error = "not found, or the body has not synced to this node";
                    return;
                }
                bytes = *data;
                filename = "download";
                for (const zetta::data::LargeObject& object : database->large_objects().list())
                    if (object.id == id) {
                        filename = object.name;
                        break;
                    }
            });
        if (user_id.empty()) {
            const std::string body = R"({"error":"unauthorized"})";
            route.status(http::status::unauthorized);
            route.res.set(http::field::content_type, "application/json");
            stamp_response_headers(route, body);
            auto [send_error] = co_await route.send(body);
            if (send_error) co_return http::route_error(send_error);
            co_return http::route_done;
        }
        if (!error.empty()) {
            const std::string body = nlohmann::json{{"error", error}}.dump();
            route.status(http::status::not_found);
            route.res.set(http::field::content_type, "application/json");
            stamp_response_headers(route, body);
            auto [send_error] = co_await route.send(body);
            if (send_error) co_return http::route_error(send_error);
            co_return http::route_done;
        }
        // Sanitize the filename for the header (strip quotes / control chars).
        std::string safe_name;
        for (char c : filename)
            safe_name += (c == '"' || c == '\\' || c == '\r' || c == '\n') ? '_' : c;
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/octet-stream");
        route.res.set("Content-Disposition", "attachment; filename=\"" + safe_name + "\"");
        stamp_response_headers(route, bytes);
        auto [send_error] = co_await route.send(bytes);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /style.css — the cacheable stylesheet (linked from every page instead of inlined).
    boost::http::route_task serve_style(boost::http::route_params& route) {
        namespace http = boost::http;
        static const std::string css = web_ui_stylesheet();
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "text/css; charset=utf-8");
        route.res.set(http::field::cache_control, "public, max-age=86400");
        stamp_response_headers(route, css);
        auto [send_error] = co_await route.send(css);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /favicon.svg — the self-contained favicon.
    boost::http::route_task serve_favicon(boost::http::route_params& route) {
        namespace http = boost::http;
        static const std::string svg = web_ui_favicon();
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "image/svg+xml");
        route.res.set(http::field::cache_control, "public, max-age=604800");
        stamp_response_headers(route, svg);
        auto [send_error] = co_await route.send(svg);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }
};

ApiServer::ApiServer(Node& node, const zetta::cluster::Cluster* cluster,
                     ServerOptions options)
    : impl_(std::make_unique<Impl>(node, cluster, std::move(options))) {}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::start() {
    namespace http = boost::http;
    namespace beast2 = boost::beast2;
    namespace corosio = boost::corosio;

    Impl& impl = *impl_;
    if (impl.is_running.load()) return true;

    http::router router;
    router.use(http::cors());
    // GET / — the server-rendered web dashboard (#647, web_ui.cpp). PUBLIC route: it self-authenticates
    // from the HttpOnly `zetta_session` cookie and serves the login page when it is absent/invalid.
    // POST /login, /logout, /run are its companions — all registered BEFORE the auth gate (they do
    // their own cookie auth), so a browser navigation stays authenticated without a bearer header.
    router.add(http::method::get, "/", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_web_root(route);
    });
    router.add(http::method::post, "/login", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_web_login(route);
    });
    router.add(http::method::post, "/logout", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_web_logout(route);
    });
    router.add(http::method::post, "/run", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_web_run(route);
    });
    router.add(http::method::post, "/shell", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_web_shell(route);
    });
    router.add(http::method::post, "/graph-save", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_graph_save(route);
    });
    router.add(http::method::post, "/blob", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_blob_upload(route);
    });
    router.add(http::method::post, "/blob/delete",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_blob_delete(route);
               });
    router.add(http::method::get, "/blob/download",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_blob_download(route);
               });
    router.add(http::method::get, "/style.css", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_style(route);
    });
    router.add(http::method::get, "/favicon.svg", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_favicon(route);
    });

    // Discovery docs are PUBLIC (registered before the gate, like /mcp.md): llms.txt is meant to be
    // fetched credential-free (an agent reads it to learn how to authenticate), and the OpenAPI
    // document is the API's own spec. The capability routes + /v1/specs stay bearer-gated below.
    impl.openapi_body = openapi_document(impl.capability_registry).dump(2);
    router.add(http::method::get, "/llms.txt", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_llms(route, "index");
    });
    router.add(http::method::get, "/llms-full.txt", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_llms(route, "full");
    });
    router.add(http::method::get, "/llms/:topic", [&impl](http::route_params& route) -> http::route_task {
        std::string topic;
        for (const auto& [parameter, value] : route.params)
            if (parameter == "topic") topic = value;
        if (topic.ends_with(".txt")) topic.resize(topic.size() - 4);
        return impl.serve_llms(route, topic);
    });
    router.add(http::method::get, "/v1/openapi.json", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_openapi(route);
    });

    // GET /mcp.md — public, human/agent Markdown on how to connect an MCP client to
    // this node (mcp_docs.cpp). Registered BEFORE the /mcp mount so the exact route
    // wins. Deliberately NOT bearer-gated (you must be able to read the connect
    // instructions without already holding the token) and it NEVER prints the token.
    router.add(http::method::get, "/mcp.md",
               [&impl](http::route_params& route) -> http::route_task {
                   std::string host;
                   if (route.req.count(http::field::host)) {
                       host = std::string(route.req.at(http::field::host));
                   }
                   if (host.empty()) {
                       host = impl.options.listen_address + ":" +
                              std::to_string(impl.options.port);
                   }
                   const bool local =
                       host.starts_with("localhost") || host.starts_with("127.") ||
                       host.starts_with("0.0.0.0") || host.starts_with("[::1]") ||
                       host.starts_with("::1");
                   const std::string origin =
                       std::string(local ? "http://" : "https://") + host;
                   // Auth is always required (#799), so the connect docs always
                   // show the bearer step.
                   const std::string body = render_mcp_md(origin, /*auth=*/true);
                   route.status(http::status::ok);
                   route.res.set(http::field::content_type,
                                 "text/markdown; charset=utf-8");
                   stamp_response_headers(route, body);
                   auto [send_error] = co_await route.send(body);
                   if (send_error) co_return http::route_error(send_error);
                   co_return http::route_done;
               });

    // The ONE auth gate (#745, epic #743): a single middleware at the router
    // seam that runs before EVERY gated route (cors handles the CORS preflight
    // before it, so credential-less OPTIONS is not blocked). Auth is ALWAYS on
    // (#799) — there is exactly one mode, no toggle, no legacy shared secret, no
    // anonymous path. The bearer MUST resolve, through the token store (#744:
    // salted-hash + constant-time compare, honoring expiry/revocation and
    // requiring an active user), to a user; anything else is 401. The resolved
    // USER is the acting identity, and downstream capability checks run against
    // its effective permissions. (The public web-UI shell + connect docs are
    // registered BEFORE this middleware, so they stay reachable without a token;
    // and the in-process headed GUI authorizes locally, not through this gate.)
    router.use([&impl](http::route_params& route) -> http::route_task {
        const std::string bearer = bearer_of(route);
        RequestIdentity identity;
        bool authorized = false;

        std::string resolved_user_id;
        // authenticate_token touches the Node (token store) — main-thread only,
        // so marshal it onto the owning loop, like every handler.
        co_await impl.bridge.run_on_owner([&] {
            if (auto user = impl.node.authenticate_token(bearer)) {
                resolved_user_id = user->id();
            }
        });
        if (!resolved_user_id.empty()) {
            authorized = true;
            identity.acting_key = std::move(resolved_user_id);
            identity.is_user = true;
        }

        if (!authorized) {
            route.status(http::status::unauthorized);
            const std::string body = transport_error_body(
                "unauthorized", "missing or invalid bearer token");
            route.res.set(http::field::content_type, "application/json");
            stamp_response_headers(route, body);
            auto [send_error] = co_await route.send(body);
            (void)send_error;
            co_return http::route_done;
        }

        // Hand the resolved identity to the matched handler. route_data is
        // cleared per request by the worker; overwrite defensively regardless.
        if (RequestIdentity* slot = route.route_data.find<RequestIdentity>()) {
            *slot = std::move(identity);
        } else {
            route.route_data.emplace<RequestIdentity>(std::move(identity));
        }
        co_return http::route_next;
    });

    router.use("/mcp", [&impl](http::route_params& route) -> http::route_task {
        if (route.req.method() != http::method::post) {
            route.status(http::status::method_not_allowed);
            stamp_response_headers(route, "POST /mcp only");
            auto [send_error] = co_await route.send("POST /mcp only");
            (void)send_error;
            co_return http::route_done;
        }

        // Auth already ran at the router-seam gate above; read the identity it
        // resolved (a user id under --require-auth, else the presented bearer).
        const RequestIdentity identity = identity_of(route);

        std::string traceparent;
        if (route.req.count("traceparent") != 0) {
            traceparent = std::string(route.req.at("traceparent"));
        }

        http::json_sink body_sink;
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, body_sink);
        (void)bytes_read;
        std::string response_body;
        if (read_error) {
            response_body =
                jsonrpc_error({}, -32700, "could not read the request body");
        } else {
            nlohmann::json request = to_nlohmann(body_sink.release());
            if (request.is_discarded()) {
                response_body = jsonrpc_error({}, -32700, "parse error");
            } else {
                // Marshal the whole dispatch onto the loop that owns the
                // Node; this coroutine suspends until drain() has run it.
                co_await impl.bridge.run_on_owner([&] {
                    response_body = impl.dispatch(
                        request, identity.acting_key, identity.is_user,
                        traceparent);
                });
            }
        }

        if (response_body.empty()) {
            route.status(http::status::accepted);  // notification: no reply
            stamp_response_headers(route, "");
            auto [send_error] = co_await route.send("");
            (void)send_error;
            co_return http::route_done;
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, response_body);
        auto [send_error] = co_await route.send(response_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    });

    // /llms.txt, /llms-full.txt, /llms/<topic>, and /v1/openapi.json are registered as PUBLIC
    // discovery docs BEFORE the auth gate above (near /favicon.svg) — llms.txt (#645) and the OpenAPI
    // document (#618) are meant to be fetched credential-free. The capability routes + /v1/specs below
    // stay bearer-gated.
    // The spec index (#646, docs/api/specs-index.md): GET /v1/specs (the
    // index) and GET /v1/specs/<name> (each raw document) — the OpenAPI
    // document + every capability and boundary schema, one navigable set.
    // Registered before the capability loop so no future capability name can
    // shadow the /v1/specs prefix.
    router.add(http::method::get, "/v1/specs",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_specs(route, "");
               });
    router.add(http::method::get, "/v1/specs/:name",
               [&impl](http::route_params& route) -> http::route_task {
                   std::string name;
                   for (const auto& [parameter, value] : route.params) {
                       if (parameter == "name") name = value;
                   }
                   return impl.serve_specs(route, name);
               });
    for (const auto& capability : impl.capability_registry.list()) {
        const auto method = rest_method(*capability) == "GET"
                                ? http::method::get
                                : http::method::post;
        router.add(method, rest_path(*capability),
                   [&impl, capability](http::route_params& route)
                       -> http::route_task {
                       return impl.serve_rest_capability(route, capability);
                   });
    }

    // The governed serving routes (#648, docs/api/serving.md): per-entity
    // endpoints for EXPOSED models and indexes. Registered unconditionally —
    // whether a given entity's route answers is decided per request from its
    // replicated `exposed` flag (serving.cpp), so un-exposing kills the route
    // on every node with no route table to rebuild.
    router.add(http::method::post, "/v1/models/:name/predict",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_entity_endpoint(route, /*predict=*/true);
               });
    router.add(http::method::get, "/v1/indexes/:name/search",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_entity_endpoint(route, /*predict=*/false);
               });

    impl.server = std::make_unique<beast2::http_server>(
        impl.io_context, impl.options.worker_coroutines, std::move(router),
        http::make_parser_config(http::parser_config(true)),
        http::make_serializer_config(http::serializer_config()));

    corosio::ipv4_address address(impl.options.listen_address);
    auto bind_error = impl.server->bind(
        corosio::endpoint(address, impl.options.port));
    if (bind_error) {
        impl.failure_reason = "bind failed: " + bind_error.message();
        return false;
    }
    impl.bound_port = impl.server->local_endpoint().port();
    impl.server->start();
    impl.io_thread = std::thread([&impl] { impl.io_context.run(); });
    impl.is_running.store(true);
    return true;
}

void ApiServer::stop() {
    Impl& impl = *impl_;
    if (!impl.is_running.exchange(false)) return;
    impl.server->stop();
    impl.io_context.stop();
    if (impl.io_thread.joinable()) impl.io_thread.join();
    // Unblock any coroutine still parked on the bridge.
    impl.bridge.drain();
}

bool ApiServer::running() const { return impl_->is_running.load(); }
unsigned short ApiServer::port() const { return impl_->bound_port; }
const std::string& ApiServer::reason() const { return impl_->failure_reason; }
void ApiServer::drain() {
    impl_->bridge.drain();
    impl_->background.prune();  // finished job workers join + free here
}

void ApiServer::wait_for_work(std::chrono::milliseconds max_idle) {
    impl_->bridge.wait_for_work(max_idle);
}

void ApiServer::set_wakeup(std::function<void()> wakeup) {
    impl_->bridge.set_wakeup(std::move(wakeup));
}
Registry& ApiServer::registry() { return impl_->capability_registry; }
zetta::observability::SpanRecorder& ApiServer::recorder() {
    return impl_->node.span_recorder();
}
void ApiServer::set_provisioner_resolver(ProvisionerResolver resolver) {
    impl_->provisioners = std::move(resolver);
}
void ApiServer::set_activity_hook(ActivityHook hook) {
    impl_->capability_registry.set_activity_hook(std::move(hook));
}

}  // namespace zetta::api
