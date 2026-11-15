// The ONE Beast2 adapter TU (docs/api/registry-design.md → Placement + build;
// docs/research/beast2-http.md). No beast2 / cppalliance-http / Boost.JSON
// name may appear outside this file; Boost.JSON <-> nlohmann conversion
// happens exactly once, here, at the wire boundary.
#include "zetta/api/server.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <map>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include <boost/beast2/http_worker.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/io/push_to.hpp>
#include <boost/capy/when_any.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/http/field.hpp>
#include <boost/http/json/json_sink.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/server/cors.hpp>
#include <boost/http/server/router.hpp>
#include <boost/json.hpp>
#include <boost/url/parse.hpp>

#include "zetta_web_font_mono_400.hpp"
#include "zetta_web_font_sans_400.hpp"
#include "zetta_web_font_sans_500.hpp"
#include "zetta_web_font_sans_600.hpp"

#include "zetta/api/background.hpp"
#include "zetta/api/clerk_auth.hpp"
#include "zetta/api/bridge.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/llms.hpp"
#include "zetta/api/mcp_docs.hpp"
#include "zetta/api/mcp_prompts.hpp"
#include "zetta/dataset/dataset_generator_factory.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/governance/credential_redaction.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/api/web_ui.hpp"
#include "zetta/agent/agent_store.hpp"
#include "object_routes.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/failed_auth_limiter.hpp"
#include "zetta/governance/scope.hpp"
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

// The largest request body the server reads (#1008). The HTTP parser's server
// default is 64 KiB, which is fine for every call but one: `upload_dataset`
// carries a file's text, up to kMaxDatasetContentBytes (16 MiB), and JSON string
// escaping can grow it — a CSV's newlines and quotes each become two bytes. Twice
// the content cap plus room for the rest of the envelope keeps every legal upload
// under the limit, and the limit still bounds what one request can make the
// node buffer.
constexpr std::uint64_t kMaxRequestBodyBytes =
    2 * zetta::data::kMaxDatasetContentBytes + 1024 * 1024;

// Boost.JSON -> nlohmann, via serialization (the one sanctioned crossing).
nlohmann::json to_nlohmann(const boost::json::value& value) {
    return nlohmann::json::parse(boost::json::serialize(value),
                                 /*cb=*/nullptr, /*allow_exceptions=*/false);
}

// Serialize a response body for the wire WITHOUT throwing. nlohmann's plain
// dump() throws on a string that is not valid UTF-8, and a value a handler
// returns can carry one (a customer's CSV column, a Latin-1 account name). A
// throw here escapes the route coroutine, the worker drops the connection, and
// the caller sees "Empty reply from server" instead of an answer (#1007).
// Invalid sequences are replaced with U+FFFD, so the rest of the body survives.
std::string wire_dump(const nlohmann::json& value) {
    return value.dump(-1, ' ', /*ensure_ascii=*/false,
                      nlohmann::json::error_handler_t::replace);
}

std::string jsonrpc_error(const nlohmann::json& id, int code,
                          std::string_view message) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id.is_null() ? nlohmann::json() : id;
    response["error"] = {{"code", code}, {"message", message}};
    return wire_dump(response);
}

std::string jsonrpc_result(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return wire_dump(response);
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
    if (name == "promote_dataset") return "Make durable";
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
        "Orama Studio console \xE2\x80\x94 one box, auto-detected.",
        "  type SQL directly        SELECT * FROM system.tables",
        "  type Python directly     model = rank(customers)",
        "  /sql <statement>         force SQL",
        "  /py <code>               force Python (compiled, gated, audited)",
        "  /tables                  list the system.* tables",
        "  /help                    this message",
        "  /clear                   clear the transcript (browser)",
        "SQL runs through the query engine; Python lowers to a Pipeline, passes",
        "Verification, then runs \xE2\x80\x94 every statement is audited."})}}.dump();
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

// Bind browser credentials to this request's exact scheme and authority. The
// ingress must preserve Host and overwrite X-Forwarded-Proto when terminating TLS.
bool trusted_browser_origin(const boost::http::route_params& route, bool allow_read_metadata = false) {
    namespace http = boost::http;
    if (route.req.count(http::field::host) != 1) return false;
    if (route.req.count("x-forwarded-proto") > 1) return false;
    if (route.req.count("x-forwarded-proto") == 1) {
        const std::string_view proto = route.req.at("x-forwarded-proto");
        if (proto != "http" && proto != "https") return false;
    }
    const std::string expected = std::string(request_is_https(route) ? "https://" : "http://") +
                                 std::string(route.req.at(http::field::host));
    const auto target = boost::urls::parse_uri(expected);
    if (!target || !target->has_authority() || target->encoded_host().empty() ||
        target->has_userinfo() || !target->encoded_path().empty() || target->has_query() ||
        target->has_fragment()) return false;

    // A sibling origin is same-site, but it must never exercise our cookies.
    if (route.req.count("sec-fetch-site") > 1) return false;
    const std::string_view site = route.req.count("sec-fetch-site") == 1
                                      ? std::string_view(route.req.at("sec-fetch-site")) : std::string_view{};
    if (route.req.count("sec-fetch-site") == 1 && site != "same-origin" && site != "none") return false;
    if (route.req.count(http::field::origin) != 0) {
        return route.req.count(http::field::origin) == 1 &&
               std::string_view(route.req.at(http::field::origin)) == expected;
    }
    const bool read = route.req.method() == http::method::get ||
                      route.req.method() == http::method::head;
    if (!allow_read_metadata || !read) return false;
    if (site == "same-origin" || site == "none") return true;
    if (route.req.count(http::field::referer) != 1) return false;
    const auto referer = boost::urls::parse_uri(route.req.at(http::field::referer));
    return referer && referer->has_authority() && !referer->has_userinfo() &&
           !referer->has_fragment() &&
           std::string(referer->scheme()) + "://" + std::string(referer->encoded_authority()) == expected;
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

boost::http::route_task refuse_browser_origin(boost::http::route_params& route) {
    namespace http = boost::http;
    route.status(http::status::forbidden);
    // The body was deliberately left unread. End the connection instead of
    // letting its remaining bytes reach another request on the worker.
    route.res.set_keep_alive(false);
    route.res.set(http::field::content_type, "application/json");
    route.res.set(http::field::cache_control, "no-store");
    const std::string body = transport_error_body(
        "forbidden", "same-origin browser request required");
    stamp_response_headers(route, body);
    auto [send_error] = co_await route.send(body);
    if (send_error) co_return http::route_error(send_error);
    co_return http::route_done;
}

// rest_status's integer mapped onto the wire enum.
boost::http::status http_status_of(int status_code) {
    switch (status_code) {
        case 200: return boost::http::status::ok;
        case 400: return boost::http::status::bad_request;
        case 403: return boost::http::status::forbidden;
        case 404: return boost::http::status::not_found;
        case 409: return boost::http::status::conflict;
        case 413: return boost::http::status::payload_too_large;
        case 416: return boost::http::status::range_not_satisfiable;
        case 422: return boost::http::status::unprocessable_entity;
        case 429: return boost::http::status::too_many_requests;
        case 503: return boost::http::status::service_unavailable;
        case 507: return boost::http::status::insufficient_storage;
        default: return boost::http::status::internal_server_error;
    }
}

// Completes without error once the client has closed or reset its end of the
// connection, and consumes no byte: a long poll races it so a client that left
// stops costing a session and a final governed read (#1174). Bytes the client
// sent ahead (a pipelined request) are not a close: the watch then ends with
// an error, which never wins a when_any race, so the poll waits on as before.
runtime::io_task<> client_left(boost::corosio::tcp_socket& socket) {
    for (;;) {
        auto [error] = co_await socket.wait(boost::corosio::wait_type::read);
        if (error) co_return runtime::io_result<>{error};
        char byte = 0;
        const auto peeked =
            ::recv(socket.native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (peeked == 0) co_return runtime::io_result<>{};
        if (peeked > 0) {
            co_return runtime::io_result<>{
                std::make_error_code(std::errc::operation_in_progress)};
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        co_return runtime::io_result<>{};  // reset or failed: the client is gone
    }
}

// A long poll's own deadline as a when_any participant.
runtime::io_task<> poll_deadline(std::chrono::milliseconds wait) {
    co_return co_await runtime::delay(wait);
}

}  // namespace

struct ApiServer::Impl {
    class RequestLifetime {
    public:
        explicit RequestLifetime(std::atomic<std::size_t>& requests)
            : requests_(requests) {
            requests_.fetch_add(1);
        }

        ~RequestLifetime() {
            requests_.fetch_sub(1);
        }

    private:
        std::atomic<std::size_t>& requests_;
    };

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

    // Each connection's request parser, by the route parameters it serves, so a
    // route can lift the body limit for its own request (#1005: an object body has
    // no cap but the bucket's quota). Touched only on the IO thread, and at
    // teardown after it has stopped.
    std::map<const boost::http::route_params*, boost::http::request_parser*> parsers;
    // Each connection's socket, by the same key, so a long poll can notice its
    // client leave (#1174). Touched only on the IO thread, like `parsers`.
    std::map<const boost::http::route_params*, boost::corosio::tcp_socket*> sockets;
    // The failed-authentication budget per remote address (#1417): a source
    // that spent it is refused before its credential is checked, so an
    // invalid-token flood cannot queue token-store work on the owner loop.
    // Touched only on the IO thread, like `parsers`.
    zetta::governance::FailedAuthLimiter auth_limiter;

    // The remote address a request arrived from: the limiter's key. The peer's
    // socket address, never a forwarded-for header a client could set.
    std::string request_source(const boost::http::route_params& route) const {
        const auto found = sockets.find(&route);
        if (found == sockets.end()) return "unknown";
        const boost::corosio::endpoint remote = found->second->remote_endpoint();
        return remote.is_v4() ? remote.v4_address().to_string()
                              : remote.v6_address().to_string();
    }

    // Resolve a credential to its user id; empty when it does not resolve.
    // Native token store first (cheap, local): authenticate_token touches the
    // Node, main-thread only, so it is marshaled onto the owning loop. Then the
    // Clerk fallback (#889): if the native token missed AND a Clerk issuer is
    // configured AND the bearer is JWT-shaped, verify it against the issuer's
    // JWKS and map the subject to a principal named clerk:<sub> (provisioned
    // on first sight). No issuer configured: this never runs (bearer-only).
    // The auth gate and the public discovery documents (#1357) share it.
    // `allow_scan` false (a source past its failed-authentication budget,
    // #1417) keeps the native check to the bounded lookups.
    runtime::task<std::string> resolve_credential(std::string bearer, bool allow_scan) {
        std::string resolved_user_id;
        co_await bridge.run_on_owner([&] {
            if (auto user = node.authenticate_token(bearer, allow_scan)) {
                resolved_user_id = user->id();
            }
        });
        if (resolved_user_id.empty() && !options.clerk_issuer.empty() &&
            looks_like_jwt(bearer)) {
            if (auto claims = co_await verify_clerk_jwt_async(options.clerk_issuer, bearer)) {
                const std::string principal_name = "clerk:" + claims->subject;
                co_await bridge.run_on_owner([&] {
                    auto user = node.find_user(principal_name);
                    if (!user) user = node.create_user(principal_name);
                    if (user) resolved_user_id = user->id();
                });
            }
        }
        co_return resolved_user_id;
    }

    // Is the caller of a public discovery document authenticated (#1357)? The
    // same credential sources as the gate (a bearer, else the session cookie
    // from a trusted origin), and the same failed-authentication budget; any
    // failure only means the anonymous rendering, never a refusal.
    runtime::task<bool> discovery_caller_authenticated(boost::http::route_params& route) {
        std::string bearer = bearer_of(route);
        if (bearer.empty()) {
            bearer = cookie_value(route, "zetta_session");
            if (!bearer.empty() && !trusted_browser_origin(route, true)) co_return false;
        }
        if (bearer.empty()) co_return false;
        const std::string source = request_source(route);
        const bool allow_scan = auth_limiter.admit(source).admitted;
        const std::string user = co_await resolve_credential(std::move(bearer), allow_scan);
        if (user.empty()) {
            auth_limiter.record_failure(source);
            co_return false;
        }
        co_return true;
    }

    // The gate's 401: no credential, or one that did not resolve to a user.
    boost::http::route_task refuse_unauthenticated(boost::http::route_params& route) {
        namespace http = boost::http;
        route.status(http::status::unauthorized);
        const std::string body =
            transport_error_body("unauthorized", "missing or invalid bearer token");
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        (void)send_error;
        co_return http::route_done;
    }

    // The gate's 429 (#1417): this source failed authentication too often.
    // The body was left unread, so the connection ends with the reply.
    boost::http::route_task refuse_rate_limited(boost::http::route_params& route,
                                                std::uint32_t retry_after_seconds) {
        namespace http = boost::http;
        route.status(http::status::too_many_requests);
        route.res.set_keep_alive(false);
        const std::string body = transport_error_body(
            "rate_limited",
            "too many failed authentications from this address; retry in " +
                std::to_string(retry_after_seconds) + " s");
        route.res.set(http::field::content_type, "application/json");
        route.res.set(http::field::retry_after, std::to_string(retry_after_seconds));
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        (void)send_error;
        co_return http::route_done;
    }

    struct Connection : boost::beast2::http_worker {
        Impl& owner;
        boost::corosio::tcp_socket socket;

        Connection(Impl& server, boost::corosio::tcp_socket accepted,
                   boost::http::router<boost::http::route_params> router,
                   boost::http::shared_parser_config parser_config,
                   boost::http::shared_serializer_config serializer_config)
            : http_worker(std::move(router), std::move(parser_config),
                          std::move(serializer_config)),
              owner(server),
              socket(std::move(accepted)) {
            rp.req_body = boost::capy::any_buffer_source(parser.source_for(socket));
            rp.res_body = boost::capy::any_buffer_sink(serializer.sink_for(socket));
            stream = boost::capy::any_read_stream(&socket);
            owner.parsers[&rp] = &parser;
            owner.sockets[&rp] = &socket;
        }
        ~Connection() {
            owner.parsers.erase(&rp);
            owner.sockets.erase(&rp);
        }
    };

    boost::corosio::io_context io_context;
    boost::corosio::tcp_acceptor acceptor{io_context};
    std::optional<boost::http::router<boost::http::route_params>> router;
    boost::http::shared_parser_config parser_config;
    boost::http::shared_serializer_config serializer_config;
    // These containers belong to the IO executor, including during shutdown.
    std::map<std::uint64_t, std::shared_ptr<Connection>> connections;
    std::vector<std::shared_ptr<Connection>> idle_connections;
    std::vector<std::weak_ptr<runtime::async_event>> poll_events;
    std::uint64_t next_connection = 0;
    std::thread io_thread;
    std::atomic<bool> is_running{false};
    std::atomic<bool> io_work_held{false};
    std::atomic<std::size_t> requests_in_flight{0};
    std::atomic<std::size_t> active_connection_count{0};
    std::atomic<std::size_t> idle_connection_count{0};
    std::atomic<std::size_t> poll_waiter_count{0};
    zetta::memory::MemoryAccounting::ProbeId memory_probe = 0;
    unsigned short bound_port = 0;
    std::string failure_reason;

    Impl(Node& owning_node, const zetta::cluster::Cluster* owning_cluster,
         ServerOptions server_options)
        : node(owning_node),
          cluster(owning_cluster),
          options(std::move(server_options)) {
        register_core_capabilities(capability_registry);
        memory_probe = node.memory_accounting().register_probe(
            "registry_sessions_connections", [this] {
                const auto active =
                    active_connection_count.load(std::memory_order_relaxed);
                const auto idle =
                    idle_connection_count.load(std::memory_order_relaxed);
                const auto waiters = poll_waiter_count.load(std::memory_order_relaxed);
                const auto requests =
                    requests_in_flight.load(std::memory_order_relaxed);
                const auto registrations = capability_registry.list().size();
                return zetta::memory::ProbeSample{
                    registrations * 768 + active * sizeof(Connection) +
                        idle * sizeof(Connection) + waiters * 128 + requests * 256,
                    registrations + active + idle + waiters + requests,
                    0};
            });
    }

    ~Impl() {
        node.memory_accounting().unregister_probe(memory_probe);
    }

    runtime::task<> serve_connection(std::uint64_t id,
                                    std::shared_ptr<Connection> connection) {
        try {
            co_await connection->do_http_session();
        } catch (const std::exception& error) {
            if (is_running.load()) {
                std::fprintf(stderr, "zetta: HTTP session failed: %s\n", error.what());
            }
        }
        connection->socket.close();
        connections.erase(id);
        active_connection_count.store(connections.size(), std::memory_order_relaxed);
        if (is_running.load() && idle_connections.size() < options.idle_connection_cache) {
            idle_connections.push_back(std::move(connection));
            idle_connection_count.store(idle_connections.size(),
                                        std::memory_order_relaxed);
        }
    }

    runtime::task<> reject_connection(boost::corosio::tcp_socket& socket) {
        const std::string body = wire_dump(rest_envelope(
            ApiResult::failure("busy", "HTTP connection limit reached")));
        const std::string response =
            "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
            "Content-Type: application/json\r\nRetry-After: 1\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;
        // This small transport-level rejection is bounded even for a peer that
        // does not read. It does not wait for any admitted session to finish.
        [[maybe_unused]] auto sent = co_await runtime::timeout(
            boost::capy::write(socket, boost::capy::const_buffer(
                response.data(), response.size())),
            std::chrono::milliseconds(50));
        socket.close();
    }

    runtime::task<> accept_connections() {
        bool reported_error = false;
        bool reported_capacity = false;
        std::size_t accepted_in_batch = 0;
        while (is_running.load()) {
            boost::corosio::tcp_socket accepted(io_context);
            auto [error] = co_await acceptor.accept(accepted);
            if (!is_running.load()) co_return;
            if (error) {
                if (!reported_error) {
                    std::fprintf(stderr, "zetta: HTTP accept failed: %s\n",
                                 error.message().c_str());
                    reported_error = true;
                }
                // Descriptor/resource exhaustion must not become a tight loop
                // that prevents existing sessions from finishing and closing.
                [[maybe_unused]] auto delayed =
                    co_await runtime::delay(std::chrono::milliseconds(10));
                continue;
            }
            reported_error = false;
            if (++accepted_in_batch == 32) {
                accepted_in_batch = 0;
                co_await runtime::yield();
                if (!is_running.load()) co_return;
            }
            if (connections.size() >= options.max_connections) {
                if (!reported_capacity) {
                    std::fprintf(stderr,
                                 "zetta: HTTP connection limit %zu reached; replying 503\n",
                                 options.max_connections);
                    reported_capacity = true;
                }
                co_await reject_connection(accepted);
                continue;
            }
            reported_capacity = false;
            std::shared_ptr<Connection> connection;
            if (idle_connections.empty()) {
                connection = std::make_shared<Connection>(
                    *this, std::move(accepted), *router, parser_config, serializer_config);
            } else {
                connection = std::move(idle_connections.back());
                idle_connections.pop_back();
                idle_connection_count.store(idle_connections.size(),
                                            std::memory_order_relaxed);
                connection->socket = std::move(accepted);
            }
            const auto id = next_connection++;
            connections.emplace(id, connection);
            active_connection_count.store(connections.size(),
                                          std::memory_order_relaxed);
            runtime::run_async(io_context.get_executor())(
                serve_connection(id, std::move(connection)));
        }
    }

    runtime::task<> stop_accepting_connections() {
        acceptor.close();
        co_return;
    }

    runtime::task<> stop_connections() {
        for (const auto& [id, connection] : connections) {
            connection->socket.close();
        }
        for (const auto& registered : poll_events) {
            if (auto event = registered.lock()) event->set();
        }
        poll_events.clear();
        poll_waiter_count.store(0, std::memory_order_relaxed);
        idle_connections.clear();
        idle_connection_count.store(0, std::memory_order_relaxed);
        co_return;
    }

    // Dispatch one JSON-RPC request document. Returns the response body, or
    // an empty string for notifications (no response required).
    std::string dispatch(const nlohmann::json& request,
                         const std::string& acting_key, bool acting_is_user,
                         const std::string& traceparent_header,
                         std::shared_ptr<zetta::runtime::async_event> wait_event = {},
                         ApiResult* observed_result = nullptr) {
        if (!request.is_object() || request.value("jsonrpc", "") != "2.0") {
            return jsonrpc_error({}, -32600, "invalid JSON-RPC 2.0 request");
        }
        const nlohmann::json id = request.value("id", nlohmann::json());
        const std::string method = request.value("method", "");

        if (method == "notifications/initialized") return {};
        if (method == "initialize") {
            return jsonrpc_result(
                id, {{"protocolVersion", kProtocolVersion},
                     {"capabilities",
                      {{"tools", nlohmann::json::object()},
                       {"prompts", nlohmann::json::object()}}},
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
                context.wait_event = wait_event;
                call_result =
                    capability_registry.call(tool_name, arguments, context);
            };
            // dispatch() itself is called from the route coroutine via the
            // bridge-aware wrapper below, so here `work` has already been
            // marshaled; see handle_mcp(). This branch never runs off-loop.
            work();
            if (observed_result) {
                *observed_result = call_result;
            }

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
                {{{"type", "text"}, {"text", wire_dump(payload)}}});
            result["structuredContent"] = std::move(payload);
            result["isError"] = !call_result.ok;
            return jsonrpc_result(id, std::move(result));
        }
        if (method == "prompts/list") {
            // Guided workflows over the tools (#872) — no per-prompt scoping: a prompt is a
            // template the client expands into tool calls, and each of those calls is still
            // scope-checked at tools/call time.
            return jsonrpc_result(id, {{"prompts", mcp_prompt_definitions()}});
        }
        if (method == "prompts/get") {
            const auto& params = request.value("params", nlohmann::json::object());
            const std::string prompt_name = params.value("name", "");
            if (prompt_name.empty()) {
                return jsonrpc_error(id, -32602, "params.name is required");
            }
            std::string reason;
            if (std::optional<nlohmann::json> prompt = render_mcp_prompt(
                    prompt_name, params.value("arguments", nlohmann::json::object()),
                    &reason)) {
                return jsonrpc_result(id, std::move(*prompt));
            }
            return jsonrpc_error(id, -32602, reason);
        }
        return jsonrpc_error(id, -32601, "unknown method '" + method + "'");
    }

    // Public discovery docs (#645, #1402): index, full, or one topic. Renderers
    // accept only the registry, so every route is independent of workspace
    // inventory. Marshal for registry access and the owner-thread span recorder.
    boost::http::route_task serve_llms(boost::http::route_params& route,
                                       std::string page) {
        namespace http = boost::http;

        std::string traceparent;
        if (route.req.count("traceparent") != 0) {
            traceparent = std::string(route.req.at("traceparent"));
        }
        const auto trace =
            zetta::observability::parse_traceparent(traceparent)
                .value_or(zetta::observability::make_root_trace());

        // Every capability for an authenticated caller; for an anonymous one
        // only the publicly callable set, which is none (#1357).
        const bool authenticated = co_await discovery_caller_authenticated(route);
        const CapabilityVisible visible =
            authenticated ? CapabilityVisible([](const Capability&) { return true; })
                          : publicly_callable();
        std::string body;
        bool found = true;
        co_await bridge.run_on_owner([&] {
            zetta::observability::ScopedSpan span(
                &node.span_recorder(), trace, "llms:" + page);
            span.set_attribute("page", page);
            span.set_attribute("authenticated", authenticated);
            if (page == "index") {
                body = render_llms_index(capability_registry, visible);
            } else if (page == "full") {
                body = render_llms_full(capability_registry, visible);
            } else {
                auto rendered =
                    render_llms_topic(capability_registry, page, visible);
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
        // The body depends on the caller's credential.
        route.res.set(http::field::vary, "Authorization, Cookie");
        route.res.set(http::field::cache_control, "private, no-cache");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // The serialized OpenAPI document — generated from the registry once at
    // start() (schema-as-code: the same generator output is drift-checked
    // into schemas/openapi.json) and served at GET /v1/openapi.json to an
    // authenticated caller. An anonymous caller gets openapi_public_body: the
    // same document over only the publicly callable capabilities (#1357).
    std::string openapi_body;
    std::string openapi_public_body;

    // Serve one REST rendering of a capability (#618, docs/api/rest-openapi.md):
    // GET for reads (input in the query string, coerced by the input schema),
    // POST for actions/jobs (input as the JSON body). Auth is enforced at the
    // router authorization filter (#745), and the call itself — validate, authorize +
    // audit, handle, span —
    // is Registry::call on the owning loop, exactly the MCP path.
    boost::http::route_task serve_rest_capability(
        boost::http::route_params& route,
        std::shared_ptr<const Capability> capability) {
        namespace http = boost::http;

        RequestLifetime request_lifetime(requests_in_flight);
        std::string response_body;
        http::status status = http::status::ok;

        // Auth is enforced once, up front, by the router authorization filter (#745); this
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
                if (capability->name == "sql" && bearer_of(route).empty()) {
                    // Classify the entire parsed batch before executing any of
                    // it. A SELECT followed by DDL must not partially execute.
                    bool read_only = false;
                    try {
                        const auto statement = input.find("statement");
                        if (statement != input.end() && statement->is_string()) {
                            const auto parsed = zetta::query::PostgresDialect{}.parse(
                                statement->get_ref<const std::string&>());
                            read_only = parsed.ok() && !parsed.statements.empty() &&
                                std::ranges::all_of(parsed.statements, [](const auto& sql) {
                                    return std::holds_alternative<zetta::query::SelectStmt>(sql);
                                });
                        }
                    } catch (...) {
                        // Parser failures cannot grant permission to execute.
                    }
                    if (!read_only) {
                        status = http::status::forbidden;
                        response_body = transport_error_body(
                            "forbidden", "cookie-authenticated GET SQL requires SELECT; use POST for writes");
                    }
                }
            } else {
                http::json_sink body_sink;
                auto [read_error, bytes_read] =
                    co_await boost::capy::push_to(route.req_body, body_sink);
                if (read_error == http::error::body_too_large) {
                    status = http::status::payload_too_large;
                    response_body = transport_error_body(
                        "invalid_input",
                        "the request body is larger than this node accepts (" +
                            std::to_string(kMaxRequestBodyBytes) + " bytes)");
                } else if (read_error) {
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

        if (!response_body.empty()) {
            route.status(status);
            route.res.set(http::field::content_type, "application/json");
            stamp_response_headers(route, response_body);
            auto [send_error] = co_await route.send(response_body);
            if (send_error) co_return http::route_error(send_error);
            co_return http::route_done;
        }
        co_return co_await answer_capability(route, std::move(capability), std::move(input),
                                             identity);
    }

    // One governed registry call on the right executor: the blocking pool for a
    // `blocks` entry, a pool worker for an offloadable one, the owning loop for
    // the rest. Every hop off the loop comes back onto it before the caller
    // touches the connection.
    runtime::task<ApiResult> call_capability(std::shared_ptr<const Capability> capability,
                                             nlohmann::json input, RequestIdentity identity,
                                             zetta::observability::TraceContext trace,
                                             std::shared_ptr<runtime::async_event> wait_event) {
        auto call = [&]() -> ApiResult {
            ApiContext context{node, cluster, identity.acting_key, trace,
                               &node.span_recorder(), &background};
            context.acting_is_user = identity.is_user;
            context.wait_event = wait_event;
            return capability_registry.call(capability->name, input, context);
        };
        ApiResult result;
        if (capability->blocks) {
            result = co_await zetta::runtime::offload_blocking(call);
            co_await bridge.run_on_owner([] {});
        } else if (capability->offloadable) {
            // Offloadable handlers (chat) touch only thread-safe state and run on
            // the pool so generation never pins the owner loop. offload resumes us
            // on a pool worker; hop back onto the owning loop before touching the
            // connection: route.send is a Corosio op bound to that loop's executor.
            result = co_await zetta::runtime::offload(call);
            co_await bridge.run_on_owner([] {});
        } else {
            co_await bridge.run_on_owner([&] { result = call(); });
        }
        co_return result;
    }

    std::shared_ptr<runtime::async_event> new_poll_event() {
        auto event = std::make_shared<zetta::runtime::async_event>();
        std::erase_if(poll_events, [](const auto& waiting) { return waiting.expired(); });
        poll_events.push_back(event);
        poll_waiter_count.store(poll_events.size(), std::memory_order_relaxed);
        return event;
    }

    void forget_poll_event(const std::shared_ptr<runtime::async_event>& event) {
        std::erase_if(poll_events, [&event](const auto& waiting) {
            return waiting.expired() || waiting.lock() == event;
        });
        poll_waiter_count.store(poll_events.size(), std::memory_order_relaxed);
    }

    // Suspend a long poll until it is woken: by its event (a publish, a topic
    // close, or stop()), by its own deadline, or by its client closing the
    // connection. Answers false only when the client has left, so the caller
    // makes no further governed read and sends nothing (#1174). Holds no thread.
    runtime::task<bool> await_poll(std::shared_ptr<runtime::async_event> event,
                                   std::chrono::milliseconds wait,
                                   const boost::http::route_params& route) {
        if (wait.count() <= 0) co_return true;
        std::vector<runtime::io_task<>> wakes;
        wakes.push_back(event->wait());
        wakes.push_back(poll_deadline(wait));
        if (const auto found = sockets.find(&route); found != sockets.end()) {
            wakes.push_back(client_left(*found->second));
        }
        const auto woken = co_await boost::capy::when_any(std::move(wakes));
        co_return !(woken.index() == 1 && std::get<1>(woken) == 2);
    }

    // Make one capability call from an already-parsed input and answer it — the
    // REST rendering's second half, shared by /v1/<name> and the object
    // capability's bucket-route aliases (#1005).
    boost::http::route_task answer_capability(boost::http::route_params& route,
                                              std::shared_ptr<const Capability> capability,
                                              nlohmann::json input, RequestIdentity identity) {
        namespace http = boost::http;
        RequestLifetime request_lifetime(requests_in_flight);
        std::string traceparent;
        if (route.req.count("traceparent") != 0) {
            traceparent = std::string(route.req.at("traceparent"));
        }
        const auto trace = zetta::observability::parse_traceparent(traceparent)
                               .value_or(zetta::observability::make_root_trace());
        ApiResult call_result;
        if (capability->awaitable_read) {
            const AwaitableRead& awaitable = *capability->awaitable_read;
            auto wait_event = new_poll_event();
            call_result = co_await call_capability(capability, input, identity, trace, wait_event);
            const auto wait_for = awaitable.wait_for(input);
            const auto deadline = std::chrono::steady_clock::now() + wait_for;
            if (!awaitable.retry_within) {
                // One wait, then one final call (#1076).
                if (awaitable.should_wait(call_result) && wait_for.count() > 0 &&
                    is_running.load()) {
                    if (!co_await await_poll(wait_event, wait_for, route)) {
                        forget_poll_event(wait_event);
                        co_return http::route_close;  // the client left (#1174)
                    }
                    call_result = co_await call_capability(capability, input, identity, trace, {});
                }
            } else {
                // Keep waiting until wait_for elapses (#1005): every wake — the
                // event, or retry_within passing — makes one more governed call.
                while (awaitable.should_wait(call_result) && is_running.load()) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now());
                    if (remaining.count() <= 0) break;
                    const auto wait = std::min(remaining, awaitable.retry_within(call_result));
                    if (!co_await await_poll(wait_event, wait, route)) {
                        forget_poll_event(wait_event);
                        co_return http::route_close;  // the client left (#1174)
                    }
                    forget_poll_event(wait_event);
                    wait_event = new_poll_event();
                    call_result =
                        co_await call_capability(capability, input, identity, trace, wait_event);
                }
            }
            forget_poll_event(wait_event);
        } else {
            call_result = co_await call_capability(capability, input, identity, trace, {});
        }
        const http::status status = http_status_of(rest_status(call_result));
        const std::string response_body = wire_dump(rest_envelope(call_result));
        route.status(status);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, response_body);
        auto [send_error] = co_await route.send(response_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // ---- the object capability's routes (#1005, docs/data/object-store.md) ----
    //
    // PUT / PATCH / GET /v1/objects/<bucket>/<key> move raw bodies, streamed a
    // slice (≤ 4 MiB) at a time in both directions; no request buffers a whole
    // object. Each is authorized and audited like a capability call
    // (authorize_object_route), and every store call runs on the blocking pool.
    // The bucket-level routes and DELETE are aliases of registry capabilities.

    // The worker reuses one response object for every request on a connection and
    // does not clear its headers (#700), so the headers only these routes set are
    // removed once their response has gone.
    static void clear_object_headers(boost::http::route_params& route) {
        route.res.erase(boost::http::field::content_range);
        route.res.erase(boost::http::field::allow);
        route.res.erase("Zetta-Generation");
    }

    // Send a JSON envelope for a finished object call.
    boost::http::route_task send_object_result(boost::http::route_params& route,
                                               const ApiResult& result,
                                               bool body_unread = false) {
        namespace http = boost::http;
        if (body_unread) {
            // A request body this route did not read would be parsed as the next
            // request: read and drop what is left of it, so the client's
            // connection stays in step, or past a bound close the connection.
            constexpr std::uint64_t kDrainBytes = 64ull * 1024 * 1024;
            std::uint64_t drained = 0;
            bool eof = false;
            while (!eof && drained <= kDrainBytes) {
                boost::capy::const_buffer chunk[16];
                auto [pull_error, buffers] = co_await route.req_body.pull(chunk);
                if (pull_error == boost::capy::error::eof) {
                    eof = true;
                    break;
                }
                if (pull_error) break;
                std::size_t consumed = 0;
                for (const boost::capy::const_buffer& buffer : buffers) consumed += buffer.size();
                route.req_body.consume(consumed);
                drained += consumed;
            }
            if (!eof) route.res.set_keep_alive(false);
        }
        const std::string body = wire_dump(rest_envelope(result));
        route.status(http_status_of(rest_status(result)));
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        clear_object_headers(route);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // Append up to `limit` bytes of the request body to `out`. `eof` is set once
    // the body is exhausted.
    runtime::task<boost::system::error_code> read_body_slice(boost::http::route_params& route,
                                                             std::string& out, std::size_t limit,
                                                             bool& eof) {
        while (out.size() < limit && !eof) {
            boost::capy::const_buffer chunk[16];
            auto [pull_error, buffers] = co_await route.req_body.pull(chunk);
            if (pull_error == boost::capy::error::eof) {
                eof = true;
                break;
            }
            if (pull_error) co_return pull_error;
            std::size_t consumed = 0;
            for (const boost::capy::const_buffer& buffer : buffers) {
                const std::size_t take = std::min(buffer.size(), limit - out.size());
                out.append(static_cast<const char*>(buffer.data()), take);
                consumed += take;
                if (out.size() == limit) break;
            }
            route.req_body.consume(consumed);
        }
        co_return boost::system::error_code{};
    }

    // Run a store call on the blocking pool. The completion event resumes this
    // coroutine on the executor it awaited from, the IO thread, so no owner-loop
    // hop follows (a byte route makes several calls per request, and each hop
    // costs a wake of the owner loop); were it ever resumed elsewhere, one hop
    // brings it back before the connection is touched.
    template <class Callable>
    runtime::task<std::invoke_result_t<Callable>> on_pool(Callable callable) {
        const auto io_thread = std::this_thread::get_id();
        auto result = co_await zetta::runtime::offload_blocking(std::move(callable));
        if (std::this_thread::get_id() != io_thread) co_await bridge.run_on_owner([] {});
        co_return result;
    }

    static std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
        std::uint64_t value = 0;
        if (text.empty()) return std::nullopt;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
        return value;
    }

    static std::string query_value(const boost::http::route_params& route, std::string_view name) {
        for (const auto& parameter : route.url.params()) {
            if (parameter.key == name) return std::string(parameter.value);
        }
        return {};
    }

    static bool query_flag(const boost::http::route_params& route, std::string_view name) {
        const std::string value = query_value(route, name);
        return value == "true" || value == "1";
    }

    // The mutation options a request carries: Zetta-Fence, Zetta-Incarnation
    // (checked by the store in the same step as the write, lake#543) and ?durable.
    static std::optional<zetta::data::ObjectWrite> write_options(
        const boost::http::route_params& route, std::string& reason) {
        zetta::data::ObjectWrite options;
        options.durable = query_flag(route, "durable");
        options.incarnation = expected_incarnation(route);
        if (route.req.count("Zetta-Fence") != 0) {
            const auto fence = parse_unsigned(route.req.at("Zetta-Fence"));
            if (!fence) {
                reason = "Zetta-Fence must be a non-negative integer";
                return std::nullopt;
            }
            options.fence = *fence;
        }
        return options;
    }

    // The Zetta-Incarnation precondition a write or barrier carries (lake#543),
    // empty when it carries none.
    static std::string expected_incarnation(const boost::http::route_params& route) {
        if (route.req.count("Zetta-Incarnation") == 0) return {};
        return std::string(route.req.at("Zetta-Incarnation"));
    }

    boost::http::route_task serve_objects(boost::http::route_params& route) {
        namespace http = boost::http;
        RequestLifetime request_lifetime(requests_in_flight);
        const RequestIdentity identity = identity_of(route);
        constexpr std::string_view kPrefix = "/v1/objects/";
        const std::string path(route.url.path());
        const std::string rest = path.size() > kPrefix.size() ? path.substr(kPrefix.size()) : "";
        const http::method method = route.req.method();
        const bool has_body = method == http::method::put || method == http::method::patch ||
                              method == http::method::post;
        const auto refuse = [&](std::string code, std::string message, std::string at = {}) {
            return send_object_result(route, ApiResult::failure(std::move(code), std::move(message),
                                                               std::move(at)),
                                      has_body);
        };
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            // The bucket-level aliases: GET <bucket>, GET <bucket>:usage,
            // POST <bucket>:barrier|:acquire|:renew|:release.
            const std::size_t colon = rest.find(':');
            const std::string bucket = rest.substr(0, colon);
            const std::string verb = colon == std::string::npos ? "" : rest.substr(colon + 1);
            if (!zetta::data::valid_bucket_name(bucket)) {
                co_return co_await refuse("invalid_input", "want a bucket name", "/bucket");
            }
            std::string name;
            if (method == http::method::get && verb.empty()) name = "list_objects";
            if (method == http::method::get && verb == "usage") name = "object_usage";
            if (method == http::method::post && verb == "barrier") name = "object_barrier";
            if (method == http::method::post && verb == "acquire") name = "acquire_object_lease";
            if (method == http::method::post && verb == "renew") name = "renew_object_lease";
            if (method == http::method::post && verb == "release") name = "release_object_lease";
            const std::shared_ptr<const Capability> capability =
                name.empty() ? nullptr : capability_registry.find(name);
            if (!capability) {
                co_return co_await send_object_result(
                    route,
                    ApiResult::failure("not_found",
                                       "no object route " + std::string(route.req.method_text()) +
                                           " " + path),
                    has_body);
            }
            nlohmann::json input = nlohmann::json::object();
            if (name == "list_objects") {
                std::vector<std::pair<std::string, std::string>> query;
                for (const auto& parameter : route.url.params()) {
                    query.emplace_back(parameter.key, parameter.value);
                }
                input = rest_input_from_query(*capability, query);
            } else if (name != "object_usage" && name != "object_barrier") {
                http::json_sink body_sink;
                auto [read_error, bytes_read] =
                    co_await boost::capy::push_to(route.req_body, body_sink);
                if (read_error) {
                    co_return co_await refuse("invalid_input", "could not read the request body as JSON");
                }
                if (bytes_read > 0) input = to_nlohmann(body_sink.release());
                if (!input.is_object()) {
                    co_return co_await send_object_result(
                        route, ApiResult::failure("invalid_input", "the request body is not a JSON object"));
                }
            } else if (has_body) {
                StringBodySink ignored;  // a barrier carries no body; drain whatever came
                auto [read_error, bytes_read] = co_await boost::capy::push_to(route.req_body, ignored);
                (void)bytes_read;
                if (read_error) co_return http::route_error(read_error);
            }
            input["bucket"] = bucket;
            if (name == "object_barrier") {
                if (std::string expected = expected_incarnation(route); !expected.empty()) {
                    input["incarnation"] = std::move(expected);
                }
            }
            co_return co_await answer_capability(route, capability, std::move(input), identity);
        }

        const std::string bucket = rest.substr(0, slash);
        const std::string key = rest.substr(slash + 1);
        if (!zetta::data::valid_bucket_name(bucket)) {
            co_return co_await refuse("invalid_input", "want a bucket name", "/bucket");
        }
        if (!zetta::data::valid_object_key(key)) {
            co_return co_await refuse("invalid_input",
                                      "want a key of 1 to 1024 bytes with no control characters",
                                      "/key");
        }
        std::string reason;
        const std::optional<zetta::data::ObjectWrite> options = write_options(route, reason);
        if (!options) co_return co_await refuse("invalid_input", reason, "/fence");
        if (method == http::method::delete_) {
            nlohmann::json input{{"bucket", bucket}, {"key", key}};
            if (options->fence) input["fence"] = *options->fence;
            if (options->durable) input["durable"] = true;
            if (!options->incarnation.empty()) input["incarnation"] = options->incarnation;
            co_return co_await answer_capability(route, capability_registry.find("delete_object"),
                                                 std::move(input), identity);
        }
        if (method != http::method::put && method != http::method::patch &&
            method != http::method::get) {
            route.res.set(http::field::allow, "GET, PUT, PATCH, DELETE");
            co_return co_await send_object_result(
                route,
                ApiResult::failure("not_found", "an object takes GET, PUT, PATCH or DELETE"),
                has_body);
        }
        zetta::data::ObjectStore* store = node.object_store();
        if (!store) {
            co_return co_await refuse("engine_error",
                                      "this node has no replicated catalogue to keep buckets in");
        }
        const std::string_view route_name = method == http::method::get     ? "get_object"
                                            : method == http::method::patch ? "put_object_range"
                                                                            : "put_object";
        std::optional<ApiResult> denied;
        co_await bridge.run_on_owner([&] {
            denied = authorize_object_route(node, identity.acting_key, identity.is_user, route_name,
                                            method == http::method::get ? kObjectsRead : kObjectsWrite,
                                            bucket);
        });
        if (denied) co_return co_await send_object_result(route, *denied, has_body);
        if (method == http::method::get) co_return co_await serve_object_get(route, *store, bucket, key);
        co_return co_await serve_object_write(route, *store, bucket, key, *options,
                                              method == http::method::patch);
    }

    boost::http::route_task serve_object_write(boost::http::route_params& route,
                                               zetta::data::ObjectStore& store, std::string bucket,
                                               std::string key, zetta::data::ObjectWrite options,
                                               bool range) {
        namespace http = boost::http;
        constexpr std::size_t kSlice = zetta::data::ObjectStore::kSliceBytes;
        std::optional<std::uint64_t> offset;
        if (range) {
            offset = parse_unsigned(query_value(route, "offset"));
            if (!offset) {
                co_return co_await send_object_result(
                    route,
                    ApiResult::failure("invalid_input", "want ?offset=<n>: where the bytes go",
                                       "/offset"),
                    true);
            }
        }
        const bool if_absent = !range && query_flag(route, "if_absent");
        std::optional<std::uint64_t> declared;
        if (route.req.count(http::field::content_length) != 0) {
            declared = parse_unsigned(route.req.at(http::field::content_length));
        }
        // The request cap is lifted only for a bucket with an effective quota
        // (its own, else the node default), which the store enforces as bytes
        // arrive (#1421). A bucket with neither keeps the request cap, refused
        // up front when the declared length already exceeds it.
        if (store.effective_quota(bucket)) {
            if (const auto parser = parsers.find(&route); parser != parsers.end()) {
                parser->second->set_body_limit(std::numeric_limits<std::uint64_t>::max());
            }
        } else if (declared && *declared > kMaxRequestBodyBytes) {
            co_return co_await send_object_result(
                route,
                ApiResult::failure("payload_too_large",
                                   "bucket '" + bucket + "' has no quota and this node sets no "
                                   "default, so a body is capped at " +
                                       std::to_string(kMaxRequestBodyBytes) +
                                       " bytes: create the bucket with quota_bytes"),
                true);
        }

        std::string slice;
        slice.reserve(std::min<std::size_t>(declared.value_or(kSlice), kSlice));
        bool eof = false;
        if (auto error = co_await read_body_slice(route, slice, kSlice, eof); error) {
            co_return http::route_error(error);
        }
        zetta::data::ObjectOutcome outcome;
        if (range) {
            // Each slice is one conditional append at the offset the previous left.
            std::uint64_t at = *offset;
            bool unchanged = true;
            for (;;) {
                outcome = co_await on_pool(
                    [&] { return store.put_range(bucket, key, at, slice, options); });
                if (!outcome.ok()) break;
                unchanged = unchanged && outcome.unchanged;
                at += slice.size();
                if (eof) break;
                slice.clear();
                if (auto error = co_await read_body_slice(route, slice, kSlice, eof); error) {
                    co_return http::route_error(error);
                }
                if (slice.empty() && eof) break;
            }
            if (outcome.ok()) outcome.unchanged = unchanged;
        } else if (eof) {
            // The whole body fit in one slice: one frame, one round.
            outcome = co_await on_pool(
                [&] { return store.put(bucket, key, slice, options, if_absent); });
        } else {
            // A larger body is staged on every owner a slice at a time, then
            // committed atomically.
            zetta::data::ObjectUpload upload;
            outcome = co_await on_pool([&] {
                return store.begin_upload(bucket, key, options, declared, upload);
            });
            while (outcome.ok()) {
                outcome = co_await on_pool([&] { return store.upload_part(upload, slice); });
                if (!outcome.ok() || eof) break;
                slice.clear();
                if (auto error = co_await read_body_slice(route, slice, kSlice, eof); error) {
                    co_await on_pool([&] {
                        store.abort_upload(upload);
                        return true;
                    });
                    co_return http::route_error(error);
                }
                if (slice.empty() && eof) break;
            }
            if (outcome.ok()) {
                outcome = co_await on_pool([&] { return store.commit_upload(upload, if_absent); });
            } else {
                co_await on_pool([&] {
                    store.abort_upload(upload);
                    return true;
                });
            }
        }
        if (!outcome.ok()) co_return co_await send_object_result(route, object_failure(outcome), !eof);
        co_return co_await send_object_result(
            route, ApiResult::success(object_write_value(bucket, outcome)));
    }

    boost::http::route_task serve_object_get(boost::http::route_params& route,
                                             zetta::data::ObjectStore& store, std::string bucket,
                                             std::string key) {
        namespace http = boost::http;
        // One range: bytes=a-b, bytes=a- or bytes=-n.
        std::uint64_t offset = 0;
        std::optional<std::uint64_t> length;
        std::optional<std::uint64_t> suffix;
        const bool ranged = route.req.count(http::field::range) != 0;
        if (ranged) {
            const std::string header(route.req.at(http::field::range));
            const std::size_t dash = header.find('-');
            if (!header.starts_with("bytes=") || dash == std::string::npos ||
                header.find(',') != std::string::npos) {
                co_return co_await send_object_result(
                    route, ApiResult::failure("invalid_input",
                                              "want one Range: bytes=a-b, bytes=a- or bytes=-n",
                                              "/range"));
            }
            const std::string first = header.substr(6, dash - 6);
            const std::string last = header.substr(dash + 1);
            const auto from = parse_unsigned(first);
            const auto to = parse_unsigned(last);
            if (first.empty() && to) {
                suffix = *to;
            } else if (from && last.empty()) {
                offset = *from;
            } else if (from && to && *to >= *from) {
                offset = *from;
                length = *to - *from + 1;
            } else {
                co_return co_await send_object_result(
                    route, ApiResult::failure("range_not_satisfiable",
                                              "the Range header names no bytes", "/range"));
            }
        }
        constexpr std::uint64_t kChunk = 1024 * 1024;
        std::shared_ptr<zetta::data::ObjectReader> reader;
        // The first chunk is read in the same pool call as the open, so a small
        // range costs one round trip. A suffix range needs the size first.
        std::string chunk;
        bool prefetched = false;
        const auto open_and_read = [&](std::uint64_t from, std::optional<std::uint64_t> span) {
            zetta::data::ObjectOutcome opened = store.open_read(bucket, key, from, span, reader);
            if (opened.ok()) {
                prefetched = reader->read(0, std::min(kChunk, reader->length()), chunk);
            }
            return opened;
        };
        // A copy this node holds is resolved and its first chunk read right here:
        // a directory lookup and a pread from the page cache, like the asset
        // routes' small local reads. Only a read that needs a peer goes to the pool.
        zetta::data::ObjectOutcome outcome;
        bool needs_peer = false;
        if (!suffix) {
            outcome = store.open_read(bucket, key, offset, length, reader, &needs_peer);
            if (outcome.ok()) {
                prefetched = reader->read(0, std::min(kChunk, reader->length()), chunk);
            }
        }
        if (suffix || needs_peer) {
            outcome = co_await on_pool([&] {
                return suffix ? store.open_read(bucket, key, 0, std::nullopt, reader)
                              : open_and_read(offset, length);
            });
        }
        if (outcome.ok() && suffix) {
            const std::uint64_t size = outcome.object->size;
            if (*suffix == 0) {  // the last zero bytes: no bytes at all
                outcome = zetta::data::ObjectOutcome{};
                outcome.status = zetta::data::ObjectStatus::OutOfRange;
                outcome.message = "the range names no bytes of object '" + key + "'";
                outcome.path = "/range";
                outcome.object = zetta::data::ObjectInfo{key, size, 0, 0};
            } else {
                offset = size - std::min(size, *suffix);
                length = size - offset;
                outcome = co_await on_pool([&] { return open_and_read(offset, length); });
            }
        }
        if (!outcome.ok()) {
            if (outcome.status == zetta::data::ObjectStatus::OutOfRange && outcome.object) {
                route.res.set(http::field::content_range,
                              "bytes */" + std::to_string(outcome.object->size));
            }
            co_return co_await send_object_result(route, object_failure(outcome));
        }
        const std::uint64_t total = reader->info().size;
        const std::uint64_t count = reader->length();
        route.status(ranged ? http::status::partial_content : http::status::ok);
        route.res.set(http::field::content_type, "application/octet-stream");
        route.res.set("Zetta-Generation", std::to_string(reader->info().generation));
        if (ranged) {
            route.res.set(http::field::content_range,
                          count == 0 ? "bytes */" + std::to_string(total)
                                     : "bytes " + std::to_string(reader->offset()) + "-" +
                                           std::to_string(reader->offset() + count - 1) + "/" +
                                           std::to_string(total));
        }
        route.res.erase(http::field::etag);
        route.res.erase(http::field::content_length);
        route.res.set_payload_size(count);
        // Stream a slice at a time: memory stays flat whatever the object's size.
        for (std::uint64_t from = 0; from < count; from += kChunk) {
            const std::uint64_t take = std::min(kChunk, count - from);
            const bool read = from == 0 ? prefetched
                                        : co_await on_pool([&] { return reader->read(from, take, chunk); });
            if (!read) {
                // The headers are gone already: all that is left is to cut the stream.
                co_return http::route_error(
                    boost::system::error_code(EIO, boost::system::generic_category()));
            }
            auto [write_error, written] =
                co_await route.res_body.write(boost::capy::const_buffer(chunk.data(), chunk.size()));
            (void)written;
            if (write_error) co_return http::route_error(write_error);
        }
        auto [eof_error] = co_await route.res_body.write_eof();
        clear_object_headers(route);
        route.res.erase(http::field::content_type);
        if (eof_error) co_return http::route_error(eof_error);
        co_return http::route_done;
    }

    // The last handler on the router: a request NO route answered (#1007).
    //
    // Without it the router falls through, the beast2 worker writes nothing, and
    // goes back to reading the next request on the same connection — so a client
    // that asked for a path this node does not serve waits forever for a response
    // that is never coming (curl: 0 bytes, then its own timeout; through a proxy
    // that closes the upstream, "Empty reply from server"). That reads exactly
    // like a hung node, and it is what an image built before a capability existed
    // shows for that capability's route. Every request gets an answer:
    //
    //   - /v1/<name> where <name> IS a capability, wrong method -> 405 naming the
    //     method it takes (reads are GET, actions/jobs are POST — #954 was filed on
    //     this exact misreading);
    //   - /v1/<name> where <name> is not a capability -> 404 unknown_capability;
    //   - anything else -> 404 not_found.
    //
    // The connection is closed after the reply: a request body this handler never
    // read would otherwise be parsed as the next request.
    boost::http::route_task serve_unrouted(boost::http::route_params& route) {
        namespace http = boost::http;

        const std::string path(route.url.path());
        const std::string method(route.req.method_text());
        constexpr std::string_view kCapabilityPrefix = "/v1/";

        http::status status = http::status::not_found;
        std::string body;
        if (path.starts_with(kCapabilityPrefix)) {
            const std::string name = path.substr(kCapabilityPrefix.size());
            std::shared_ptr<const Capability> capability;
            co_await bridge.run_on_owner(
                [&] { capability = capability_registry.find(name); });
            if (capability) {
                const std::string expected = rest_method(*capability);
                status = http::status::method_not_allowed;
                route.res.set(http::field::allow, expected);
                body = transport_error_body(
                    "method_not_allowed",
                    "'" + name + "' is " +
                        (expected == "GET"
                             ? std::string("a read: call it with GET and the input in "
                                           "the query string")
                             : std::string("an action: call it with POST and the "
                                           "input as a JSON body")) +
                        " (got " + method + ")");
            } else {
                body = transport_error_body(
                    "unknown_capability",
                    "no capability named '" + name +
                        "' on this node — GET /v1/openapi.json lists the ones it "
                        "serves (an older node may predate the one you asked for)");
            }
        } else {
            body = transport_error_body("not_found",
                                        "no route for " + method + " " + path);
        }

        route.status(status);
        route.res.set(http::field::content_type, "application/json");
        route.res.set_keep_alive(false);
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /v1/specs and /v1/specs/<name> (#646, docs/api/specs-index.md): the
    // spec index and each raw document — the OpenAPI document, capability
    // input/output schemas, boundary schemas — served from the C++ accessors,
    // never from disk. Auth is enforced at the router authorization filter (#745);
    // rendering only reads the (start-frozen) registry and static accessors,
    // but is marshaled onto the
    // owning loop so the span lands in the node's ring (every operation is
    // observable). `name` empty = the index.
    boost::http::route_task serve_specs(boost::http::route_params& route,
                                        std::string name) {
        namespace http = boost::http;

        // Auth is enforced once, up front, by the router authorization filter (#745).
        std::string traceparent;
        if (route.req.count("traceparent") != 0) {
            traceparent = std::string(route.req.at("traceparent"));
        }
        const auto trace =
            zetta::observability::parse_traceparent(traceparent)
                .value_or(zetta::observability::make_root_trace());

        std::string body;
        bool found = true;
        const RequestIdentity identity = identity_of(route);
        co_await bridge.run_on_owner([&] {
            zetta::observability::ScopedSpan span(
                &node.span_recorder(), trace,
                name.empty() ? "specs:index" : "specs:" + name);
            span.set_attribute("spec", name.empty() ? "index" : name);
            // Only the capabilities the caller may call (#766), the rule
            // tools/list applies.
            const CapabilityVisible visible =
                callable_by(node, identity.acting_key, identity.is_user);
            if (name.empty()) {
                body = spec_index(capability_registry, visible).dump(2);
            } else {
                const auto spec = find_spec(capability_registry, name, visible);
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
    // search route (GET, query string). Auth is enforced at the router authorization
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

        // Auth is enforced once, up front, by the router authorization filter (#745); read
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

    // Serve a published pipeline (zetta#1289, docs/api/serving.md): POST
    // /v1/pipelines/{name}/run runs its pinned version with the JSON body as the
    // declared input; GET /v1/pipelines/{name}/runs/{run_id} answers a run it
    // served to the same caller. Governance, limits, audit and the run itself are
    // the HTTP-free serving layer on the owning loop. The synchronous mode is
    // this route's: after the run starts it waits, on a timer that holds no
    // thread, for the run to finish within the pipeline's sync_timeout_ms
    // (`?wait=false` skips it), then answers its outputs (200), or its id and
    // status endpoint while it still runs (202).
    boost::http::route_task serve_pipeline_endpoint(boost::http::route_params& route,
                                                    bool status_read) {
        namespace http = boost::http;
        const RequestIdentity identity = identity_of(route);
        std::string target;
        std::string run_id;
        for (const auto& [parameter, value] : route.params) {
            if (parameter == "name") target = value;
            if (parameter == "run_id") run_id = value;
        }
        bool wait = true;
        for (const auto& parameter : route.url.params()) {
            if (parameter.key == "wait" && parameter.value == "false") wait = false;
        }

        std::string response_body;
        http::status status = http::status::ok;
        nlohmann::json input = nlohmann::json::object();
        if (status_read) {
            input = {{"run_id", run_id}};
        } else {
            http::json_sink body_sink;
            auto [read_error, bytes_read] =
                co_await boost::capy::push_to(route.req_body, body_sink);
            if (read_error) {
                status = http::status::bad_request;
                response_body = transport_error_body(
                    "invalid_input", "could not read the request body as JSON");
            } else if (bytes_read > 0) {
                input = to_nlohmann(body_sink.release());
                if (input.is_discarded()) {
                    status = http::status::bad_request;
                    response_body = transport_error_body(
                        "invalid_input", "the request body is not valid JSON");
                }
            }
        }

        if (response_body.empty()) {
            std::string traceparent;
            if (route.req.count("traceparent") != 0) {
                traceparent = std::string(route.req.at("traceparent"));
            }
            const auto trace = zetta::observability::parse_traceparent(traceparent)
                                   .value_or(zetta::observability::make_root_trace());
            ServeRequest request{target, input, identity.acting_key, trace,
                                 &node.span_recorder()};
            request.acting_is_user = identity.is_user;
            request.registry = &capability_registry;
            request.background = &background;
            request.cluster = cluster;
            ApiResult result;
            std::uint32_t sync_timeout_ms = 0;
            co_await bridge.run_on_owner([&] {
                result = status_read ? serve_pipeline_run_status(node, request)
                                     : serve_pipeline_run(node, request);
                PublishedPipeline published;
                if (!status_read && result.ok &&
                    resolve_published_pipeline(node, target, published).ok) {
                    sync_timeout_ms = published.graph->exposure().sync_timeout_ms;
                }
            });
            if (!status_read && result.ok && wait && !pipeline_run_finished(result)) {
                const std::string started = result.value.value("run_id", std::string{});
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(sync_timeout_ms);
                bool finished = false;
                while (!finished && std::chrono::steady_clock::now() < deadline) {
                    [[maybe_unused]] auto waited =
                        co_await runtime::delay(std::chrono::milliseconds(50));
                    co_await bridge.run_on_owner([&] {
                        const auto run = node.find_run(started);
                        finished = !run ||
                                   run->status() == zetta::pipeline::RunStatus::Succeeded ||
                                   run->status() == zetta::pipeline::RunStatus::Failed;
                    });
                }
                if (finished) {
                    co_await bridge.run_on_owner([&] {
                        ServeRequest read = request;
                        read.input = {{"run_id", started}};
                        ApiResult answered = serve_pipeline_run_status(node, read);
                        if (answered.ok) {
                            answered.value["status_endpoint"] =
                                result.value.value("status_endpoint", std::string{});
                            answered.value["project"] = result.value.value("project", std::string{});
                            answered.value["name"] = result.value.value("name", std::string{});
                            result = std::move(answered);
                        }
                    });
                }
            }
            status = http_status_of(rest_status(result));
            if (result.ok && !pipeline_run_finished(result)) status = http::status::accepted;
            response_body = rest_envelope(result).dump();
        }

        route.status(status);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, response_body);
        auto [send_error] = co_await route.send(response_body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /v1/openapi.json — the OpenAPI rendering, served before the auth
    // gate: every capability to an authenticated caller, the publicly callable
    // ones (none) to an anonymous caller (#1357).
    boost::http::route_task serve_openapi(boost::http::route_params& route) {
        namespace http = boost::http;
        const bool authenticated = co_await discovery_caller_authenticated(route);
        const std::string& body = authenticated ? openapi_body : openapi_public_body;
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        route.res.set(http::field::vary, "Authorization, Cookie");
        route.res.set(http::field::cache_control, "private, no-cache");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
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
    // The served agents (agent:<id>) as a picker, so the Chat form is a selector (#849).
    std::string agent_options(ApiContext& context) {
        std::string options = "<option value=\"\">select an agent\xE2\x80\xA6</option>";
        if (context.node.replication() != nullptr)
            for (const zetta::agent::AgentRecord& agent :
                 zetta::agent::AgentStore(*context.node.replication()).list())
                options += form_option(agent.name.empty() ? agent.id : agent.name, agent.id,
                                       false);
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
    // that is what the schema gives us — by the one rule every surface shares
    // (governance/credential_redaction.hpp, #74).
    static bool is_secret_property(const std::string& key) {
        return zetta::governance::is_credential_field(key);
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
        if (name == "promote_dataset") return web_ui_icon("dataset");
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
            } else if (key == "agent") {
                options = agent_options(context);
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

    // The web console's consent approval surface (#999): every PENDING challenge
    // as a card naming exactly what would be read and run, with Approve / Deny
    // forms that POST /run as `approve_consent` / `deny_consent`. Nothing here
    // decides anything itself: the form goes through Registry::call like MCP and
    // REST, so the engine authorizes the signed-in user (the
    // `orchestration:approve_consent` right, from `cloud:manage`), refuses an
    // approval from the requester (decide_consent's different-principal rule)
    // and audits the decision as `consent_decided`. The list is read through
    // `list_consent_requests` under the same identity, so a viewer who may not
    // read the ledger sees the engine's refusal, never the rows.
    std::string render_consent_requests(ApiContext& context) {
        std::string out;
        if (!context.node.config().enforce_consent) {
            out += "<div class=\"bannerrow warnb\"><span class=\"bico\">" +
                   web_ui_icon("governance") +
                   "</span><span class=\"bbody\"><b>Consent gating is off on this "
                   "node</b><span>Cloud-touching calls run without approval and park "
                   "nothing here; each one is still audited as consent_bypassed. "
                   "Start the node with --governance (or ZETTA_GOVERNANCE=1) "
                   "to require an approval.</span></span></div>";
        }
        const ApiResult listed = capability_registry.call(
            "list_consent_requests", nlohmann::json::object(), context);
        if (!listed.ok) {
            return out + web_ui_panel("Pending approvals", "",
                                      "<div class=\"err-box\">" +
                                          web_ui_escape(listed.error.code) + ": " +
                                          web_ui_escape(listed.error.message) + "</div>");
        }
        const auto text = [](const nlohmann::json& object, const char* key) {
            const auto found = object.find(key);
            return found != object.end() && found->is_string() ? found->get<std::string>()
                                                                : std::string{};
        };
        const std::string return_to = "/?view=governance&amp;tab=consent";
        std::string cards;
        std::size_t pending = 0;
        for (const auto& challenge :
             listed.value.value("consent_requests", nlohmann::json::array())) {
            if (!challenge.is_object() || text(challenge, "status") != "pending") continue;
            ++pending;
            const std::string id = text(challenge, "id");
            const std::string requested_by = text(challenge, "requested_by");
            const nlohmann::json action = challenge.value("action", nlohmann::json::object());
            std::string card = "<div class=\"consentcard\" data-challenge=\"" +
                               web_ui_escape(id) + "\"><div class=\"consenthd\">"
                               "<span class=\"pill warn\">Approval required</span>"
                               "<span class=\"mono\">" +
                               web_ui_escape(text(challenge, "capability")) +
                               "</span></div><p>" + web_ui_escape(text(action, "summary")) +
                               "</p><div class=\"kvs\"><div class=\"kv\"><span>Requested by"
                               "</span><b class=\"mono\">" + web_ui_escape(requested_by) +
                               "</b></div><div class=\"kv\"><span>Expires</span><b "
                               "class=\"mono\">" + web_ui_escape(text(challenge, "expires_at")) +
                               "</b></div><div class=\"kv\"><span>Challenge</span><b "
                               "class=\"mono\">" + web_ui_escape(id) + "</b></div></div>";
            const auto list = [&](const char* key, const char* label) {
                const auto found = action.find(key);
                if (found == action.end() || !found->is_array() || found->empty()) return;
                card += "<div class=\"eyebrow\">" + std::string(label) + "</div><ul class=\"mono\">";
                for (const auto& item : *found)
                    if (item.is_string())
                        card += "<li>" + web_ui_escape(item.get<std::string>()) + "</li>";
                card += "</ul>";
            };
            list("reads", "May read");
            list("runs", "Would run");
            const std::string hidden =
                "<input type=\"hidden\" name=\"challenge_id\" value=\"" + web_ui_escape(id) +
                "\"><input type=\"hidden\" name=\"_return\" value=\"" + return_to + "\">";
            card += "<div class=\"consentact\">";
            // The requester can never approve its own action, so it is not
            // offered one; the engine refuses it anyway if posted by hand.
            if (requested_by == context.acting_key) {
                card += "<span class=\"q\">You requested this; a different approver must "
                        "approve it. You can withdraw it.</span>";
            } else {
                card += "<form method=\"post\" action=\"/run\" class=\"inline\">"
                        "<input type=\"hidden\" name=\"cap\" value=\"approve_consent\">" +
                        hidden +
                        "<button class=\"btn btn-primary small\">Approve and run once"
                        "</button></form>";
            }
            card += "<form method=\"post\" action=\"/run\" class=\"inline\">"
                    "<input type=\"hidden\" name=\"cap\" value=\"deny_consent\">" +
                    hidden +
                    "<input type=\"text\" name=\"reason\" placeholder=\"Reason (optional)\" "
                    "aria-label=\"Reason\"><button class=\"btn small\">" +
                    std::string(requested_by == context.acting_key ? "Withdraw" : "Deny") +
                    "</button></form></div></div>";
            cards += card;
        }
        if (pending == 0) {
            cards = "<div class=\"empty\">No pending consent requests. A cloud-touching call "
                    "parks a challenge here for a different approver to decide; approvals "
                    "are per-action and never remembered.</div>";
        }
        return out + web_ui_panel("Pending approvals",
                                  std::to_string(pending) +
                                      " waiting \xC2\xB7 approval needs cloud:manage and a "
                                      "different user than the requester",
                                  cards);
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
    // are interpolated into SQL — no injection.
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
            list + "</div><script>(function(){var db=" + web_ui_script_json(db) +
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
            // The Projects index (July 24): search, filter segments, and the
            // rich table — every column derived from live engine state.
            const std::string q = query_param(route, "q", "");
            const std::string filter = query_param(route, "f", "all");
            ApiResult r =
                capability_registry.call("list_projects", nlohmann::json::object(), context);
            // user id -> name, so the Owner column reads as a person.
            std::map<std::string, std::string> user_names;
            if (ApiResult users = capability_registry.call(
                    "sql", nlohmann::json{{"statement", "SELECT user_id, name FROM system.users"}},
                    context);
                users.ok && users.value.contains("rows") && users.value["rows"].is_array())
                for (const auto& row : users.value["rows"]) {
                    if (!row.is_object()) continue;
                    const auto id = row.find("user_id");
                    const auto name = row.find("name");
                    if (id != row.end() && id->is_string() && name != row.end() &&
                        name->is_string())
                        user_names[id->get<std::string>()] = name->get<std::string>();
                }
            // Latest run per project id, one governed read.
            struct RunInfo { std::string status; std::string updated; };
            std::map<std::string, RunInfo> latest;
            if (ApiResult runs = capability_registry.call(
                    "sql",
                    nlohmann::json{{"statement",
                                    "SELECT project, status, updated FROM system.runs"}},
                    context);
                runs.ok && runs.value.contains("rows") && runs.value["rows"].is_array())
                for (const auto& row : runs.value["rows"]) {
                    if (!row.is_object()) continue;
                    auto text = [&](const char* key) -> std::string {
                        const auto found = row.find(key);
                        return found != row.end() && found->is_string()
                                   ? found->get<std::string>()
                                   : std::string();
                    };
                    const std::string project = text("project");
                    if (project.empty()) continue;
                    RunInfo& slot = latest[project];
                    const std::string updated = text("updated");
                    if (updated >= slot.updated) slot = {text("status"), updated};
                }
            std::string rows_html;
            std::size_t total = 0;
            const nlohmann::json names =
                r.ok ? r.value.value("projects", nlohmann::json::array()) : nlohmann::json::array();
            for (const auto& n : names) {
                if (!n.is_string()) continue;
                const std::string name = n.get<std::string>();
                if (!q.empty()) {
                    std::string hay = name;
                    std::string needle = q;
                    std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
                    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
                    if (hay.find(needle) == std::string::npos) continue;
                }
                ApiResult detail = capability_registry.call(
                    "get_project", nlohmann::json{{"name", name}}, context);
                if (!detail.ok) continue;
                const std::string uuid = detail.value.value("id", "");
                std::string owner = detail.value.value("owner", "");
                if (const auto resolved = user_names.find(owner); resolved != user_names.end())
                    owner = resolved->second;
                else if (owner.size() > 12)
                    owner = owner.substr(0, 8);
                const std::size_t graphs =
                    detail.value.value("graphs", nlohmann::json::array()).size();
                const auto run_found = latest.find(uuid);
                const bool has_run = run_found != latest.end();
                const std::string run_status = has_run ? run_found->second.status : "";
                // Status / health / next action, all from engine state.
                std::string status = "Draft", stone, health = "Not evaluated", htone,
                            next = "Capture an intent", next_href = "/?view=projects&p=" + name;
                if (run_status == "Running") {
                    status = "Running"; stone = "warn"; health = "Waiting"; htone = "warn";
                    next = "Review run"; next_href = "/?view=pipeline";
                } else if (run_status == "Failed") {
                    status = "Needs attention"; stone = "err"; health = "Needs attention";
                    htone = "err"; next = "Review failure"; next_href = "/?view=pipeline";
                } else if (!run_status.empty()) {
                    status = "Ready"; stone = "ok"; health = "Healthy"; htone = "ok";
                    next = "Review results"; next_href = "/?view=pipeline";
                } else if (graphs > 0) {
                    status = "Ready"; stone = "ok"; health = "Not evaluated";
                    next = "Verify and deploy";
                }
                if (filter == "running" && status != "Running") continue;
                if (filter == "attention" && health != "Needs attention") continue;
                ++total;
                std::string updated =
                    has_run ? run_found->second.updated : detail.value.value("updated", "");
                rows_html += "<tr><td><a href=\"/?view=projects&p=" + web_ui_escape(name) +
                             "\"><b>" + web_ui_escape(name) +
                             "</b></a></td><td><span class=\"pill " + stone + "\">" + status +
                             "</span></td><td>" + web_ui_escape(owner.empty() ? "\xC2\xB7" : owner) +
                             "</td><td class=\"mono\">" +
                             (graphs > 0 ? "Compiled" : "Sandbox") +
                             "</td><td><span class=\"pill " + htone + "\">" + health +
                             "</span></td><td class=\"mono\">" +
                             web_ui_escape(updated.substr(0, 10)) + "</td><td><a href=\"" +
                             next_href + "\">" + next + " \xE2\x86\x92</a></td></tr>";
            }
            auto seg = [&](const char* id, const char* label) {
                return std::string("<a class=\"") + (filter == id ? "on" : "") +
                       "\" href=\"/?view=projects&f=" + id +
                       (q.empty() ? "" : "&q=" + q) + "\">" + label + "</a>";
            };
            std::string list =
                "<form class=\"filterrow\" method=\"get\" action=\"/\">"
                "<input type=\"hidden\" name=\"view\" value=\"projects\">"
                "<input type=\"search\" name=\"q\" placeholder=\"Search Projects\" value=\"" +
                web_ui_escape(q) +
                "\"><span class=\"filterseg\">" + seg("all", "All") +
                seg("running", "Running") + seg("attention", "Needs attention") +
                "</span><span class=\"fcount mono\">" + std::to_string(total) +
                " Projects</span></form>";
            if (rows_html.empty())
                list += "<div class=\"emptybox\"><span class=\"eico\">" +
                        web_ui_icon("projects") +
                        "</span><span class=\"et\">No projects match</span></div>";
            else
                list += "<div class=\"twrap\"><table><thead><tr><th>Project</th><th>Status</th>"
                        "<th>Owner</th><th>Environment</th><th>Health</th><th>Updated</th>"
                        "<th>Next action</th></tr></thead><tbody>" +
                        rows_html + "</tbody></table></div>";
            std::string ignored_list;
            (void)ignored_list;
            std::string out = render_action_bar(context, "projects", {"create_project"});
            out += web_ui_panel(
                "Projects",
                "an AI project: intent \xE2\x86\x92 Pipeline \xE2\x86\x92 Verification \xE2\x86\x92 "
                "deploy \xE2\x86\x92 run",
                list);
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
            "Pipelines", "what this project's intent compiled to",
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

    // The Studio home (#874 second pass) — the design's create-first welcome:
    // hero ("What would you like to create?"), the two entry paths (Create a
    // Solution · Discover Opportunities), and Continue your work (every
    // Project with its live status + the next useful action, derived from
    // engine state — never a fixture). The operator dashboard this replaced
    // lives on the Operate views (Runs and activity, Infrastructure).
    std::string render_studio_home(ApiContext& context) {
        ApiResult info =
            capability_registry.call("server_info", nlohmann::json::object(), context);
        const long long nodes =
            (info.ok && info.value.contains("cluster_members") &&
             info.value["cluster_members"].is_array())
                ? static_cast<long long>(info.value["cluster_members"].size())
                : 0;

        std::string out = web_ui_page_hero(
            "Studio home", "What would you like to create?",
            "Start with a goal, or let Studio study the data you already have.",
            "<span>Workspace <b>" +
                web_ui_escape(info.ok ? info.value.value("node_id", "").substr(0, 8) : "") +
                "</b></span><span>Nodes <b>" + std::to_string(nodes) +
                "</b></span><span>Version <b>v" +
                web_ui_escape(info.ok ? info.value.value("version", "") : "") +
                "</b></span>");

        // The two entry paths. Create a Solution is fully backed (create_project
        // → capture_intent → compile). Discover Opportunities renders honestly:
        // the Auto-Suggest model that reads a Workspace Profile (your data,
        // connectors, and daily work) is in training — no mock flow behind it.
        out += "<div class=\"paths\">";
        out += "<div class=\"path\"><div class=\"peyebrow\"><span class=\"pico\">" +
               web_ui_icon("bolt") +
               "</span>Start with a goal</div><h3>Create a Solution</h3>"
               "<div class=\"pbody\">Describe your goal. Studio proposes a Solution, builds "
               "it, verifies it against your success measures, and gets it ready for "
               "production.</div>"
               "<div class=\"pact\"><a href=\"/?view=projects\">Describe your goal " +
               web_ui_icon("chevron") + "</a></div></div>";
        out += "<div class=\"path muted\"><div class=\"peyebrow\"><span class=\"pico\">" +
               web_ui_icon("observability") +
               "</span>Start with your data</div><h3>Discover Opportunities</h3>"
               "<div class=\"pbody\">Not sure where to start? Studio studies your Workspace "
               "Profile \xE2\x80\x94 the data you hold, your connections, and your daily "
               "work \xE2\x80\x94 and recommends what you could create from it.</div>"
               "<div class=\"pnote\"><span class=\"dot\"></span>Auto-Suggest is in training "
               "\xC2\xB7 not available on this node yet</div>"
               // The design gives both entry cards an action. Ours had one, so this
               // card explained something you could not begin. The profile is real
               // and useful before Auto-Suggest ships: it is the input it will read.
               "<div class=\"pact\"><a href=\"/?view=profile\">Set up profile " +
               web_ui_icon("chevron") + "</a></div></div>";
        out += "</div>";

        // Continue your work — live Projects with the next useful action. The
        // latest run per project (system.runs) decides the state; a project
        // with no runs falls back to where it is in the authoring flow.
        struct LatestRun { std::string status; std::string started; };
        std::map<std::string, LatestRun> latest_by_project;
        ApiResult runs = capability_registry.call(
            "sql", nlohmann::json{{"statement", "SELECT project, status, started FROM system.runs"}},
            context);
        if (runs.ok && runs.value.contains("rows") && runs.value["rows"].is_array())
            for (const auto& row : runs.value["rows"]) {
                if (!row.is_object()) continue;
                // .value() throws on an explicit null — system rows carry
                // nulls, so read defensively (the no-exceptions boundary).
                auto text = [&](const char* key) -> std::string {
                    const auto found = row.find(key);
                    return found != row.end() && found->is_string()
                               ? found->get<std::string>()
                               : std::string();
                };
                const std::string project = text("project");
                const std::string started = text("started");
                if (project.empty()) continue;
                LatestRun& latest = latest_by_project[project];
                if (started >= latest.started) latest = {text("status"), started};
            }

        std::string rows;
        ApiResult projects =
            capability_registry.call("list_projects", nlohmann::json::object(), context);
        if (projects.ok)
            for (const auto& name : projects.value.value("projects", nlohmann::json::array())) {
                if (!name.is_string()) continue;
                const std::string pname = name.get<std::string>();
                ApiResult detail = capability_registry.call(
                    "get_project", nlohmann::json{{"name", pname}}, context);
                if (!detail.ok) continue;
                const std::string uuid = detail.value.value("id", "");
                const std::size_t inputs =
                    detail.value.value("inputs", nlohmann::json::array()).size();
                const std::size_t graphs =
                    detail.value.value("graphs", nlohmann::json::array()).size();
                std::string pill = "Draft", tone, next = "Capture an intent", age;
                if (auto found = latest_by_project.find(uuid);
                    found != latest_by_project.end()) {
                    const std::string status = found->second.status;
                    age = found->second.started.empty()
                              ? detail.value.value("updated", "").substr(0, 10)
                              : found->second.started.substr(0, 10);
                    if (status == "Running") { pill = "Running"; tone = "warn"; next = "Review run"; }
                    else if (status == "Failed") { pill = "Failed"; tone = "err"; next = "Review failure"; }
                    else { pill = "Succeeded"; tone = "ok"; next = "Review results"; }
                } else if (graphs > 0) {
                    pill = "Ready"; tone = "ok"; next = "Verify and deploy";
                    age = detail.value.value("updated", "").substr(0, 10);
                } else {
                    if (inputs > 0) next = "Compile the intent";
                    age = detail.value.value("updated", "").substr(0, 10);
                }
                rows += "<a class=\"workrow\" href=\"/?view=projects&p=" + web_ui_escape(pname) +
                        "\"><span class=\"wstat\"><span class=\"pill " + tone + "\">" + pill +
                        "</span><span class=\"wage\">" + web_ui_escape(age) +
                        "</span></span><span class=\"wname\">" + web_ui_escape(pname) +
                        "</span><span class=\"wnext\">" + next +
                        "</span><span class=\"warr\">" + web_ui_icon("chevron") + "</span></a>";
            }
        if (rows.empty())
            rows = "<div class=\"emptybox\"><span class=\"eico\">" + web_ui_icon("projects") +
                   "</span><span class=\"et\">No projects yet</span><span class=\"eh\">"
                   "Describe a goal to create your first Solution.</span></div>";
        out += web_ui_panel("Continue your work",
                            "active Projects and the next useful action",
                            "<div class=\"worklist\">" + rows + "</div>");
        return out;
    }

    // An explanatory legend for the pipeline model — the answer to "what's a run vs a job".
    std::string pipeline_legend() {
        return "<div class=\"legend\"><div class=\"legendhd\"><span class=\"ico\">" +
               web_ui_icon("pipeline") +
               "</span><b>How a Pipeline works</b></div>"
               "<div class=\"legendflow\">Project <span>\xE2\x86\x92</span> Intent <span>\xE2\x86\x92"
               "</span> <em>Pipeline</em> <span>\xE2\x86\x92</span> Verification <span>\xE2\x86\x92</span> Deploy "
               "<span>\xE2\x86\x92</span> <em>Run</em> <span>\xE2\x86\x92</span> <em>Jobs</em> "
               "<span>\xE2\x86\x92</span> Model</div>"
               "<div class=\"legenddefs\">"
               "<div><b>Pipeline</b> the compiled steps (connect \xE2\x86\x92 clean \xE2\x86\x92 model "
               "\xE2\x86\x92 serve) a project's intent lowers to.</div>"
               "<div><b>Run</b> one end-to-end execution of a Pipeline; produces a Model + outputs. One "
               "row per run.</div>"
               "<div><b>Job</b> a unit of native distributed compute a run fans out into (featurize / "
               "train / score) across the cluster \xE2\x80\x94 many jobs per run.</div>"
               "</div></div>";
    }

    // The Pipelines panel — aggregates every project's compiled execution graphs (there is no
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
                "Pipeline editor",
                "Technical view \xC2\xB7 the Pipeline as its raw execution graph \xC2\xB7 connect "
                "\xE2\x86\x92 clean \xE2\x86\x92 model \xE2\x86\x92 index \xE2\x86\x92 serve \xE2\x86\x92 act",
                "<div class=\"emptybox\"><span class=\"eico\">" + web_ui_icon("pipeline") +
                    "</span><span class=\"et\">No Pipelines yet</span><span class=\"eh\">Compile a "
                    "project's intent (Projects \xE2\x86\x92 open one \xE2\x86\x92 compile) to get an "
                    "editable Pipeline here.</span></div>");

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
            "<script id=\"ge-data\" type=\"application/json\">" + web_ui_script_json(data) + "</script>"
            "<script>" + std::string(kGraphEditorJs) + "</script>";
        return web_ui_panel("Pipeline editor",
                            "Technical view \xC2\xB7 the Pipeline as its raw execution graph \xC2\xB7 "
                            "connect \xE2\x86\x92 clean \xE2\x86\x92 model \xE2\x86\x92 index "
                            "\xE2\x86\x92 serve \xE2\x86\x92 act",
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
                      "</span><span class=\"et\">No Pipelines yet</span><span class=\"eh\">A Pipeline "
                      "is created when you compile a project's intent (Projects \xE2\x86\x92 open a "
                      "project \xE2\x86\x92 compile). Then verify, deploy, and run it.</span></div>"
                : "<div class=\"twrap\"><table><thead><tr><th>Pipeline</th><th>Project</th><th>id</th>"
                  "</tr></thead><tbody>" + rows + "</tbody></table></div>";
        return web_ui_panel("Pipelines",
                            "compiled Pipelines \xC2\xB7 what a project's intent lowers to",
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
            // The Studio home — create-first per the design. The workspace
            // counts moved to Runs and activity; the cluster table lives on
            // Infrastructure.
            out += render_studio_home(context);
        } else if (view == "cluster") {
            // Infrastructure (July 24): the page hero, the stat cards, an
            // unreachable-node banner when one exists, then the node tables.
            long long node_count = 0, live_count = 0;
            std::string unreachable_node;
            if (ApiResult nodes_read = capability_registry.call(
                    "sql", nlohmann::json{{"statement", "SELECT node_id, reachability FROM system.nodes"}},
                    context);
                nodes_read.ok && nodes_read.value.contains("rows"))
                for (const auto& row : nodes_read.value["rows"]) {
                    if (!row.is_object()) continue;
                    ++node_count;
                    auto text = [&](const char* key) -> std::string {
                        const auto found = row.find(key);
                        return found != row.end() && found->is_string()
                                   ? found->get<std::string>()
                                   : std::string();
                    };
                    if (text("reachability").find("unreachable") != std::string::npos)
                        unreachable_node = text("node_id");
                    else
                        ++live_count;
                }
            out += web_ui_page_hero(
                "Infrastructure", "Cluster",
                "Advanced capacity and node controls stay available without leading the "
                "main creation experience.",
                "<span>Scope <b>Advanced</b></span><span>Cluster health <b>" +
                    std::to_string(live_count) + " of " + std::to_string(node_count) +
                    " live</b></span>");
            if (!unreachable_node.empty())
                out += "<div class=\"bannerrow errb\"><span class=\"bico\">" +
                       web_ui_icon("crash") + "</span><span class=\"bbody\"><b>One node is "
                       "unreachable</b><span>Work remains available on the other nodes. Open "
                       "the node to review reachability and recovery detail.</span></span>"
                       "<span class=\"bact mono\">" +
                       web_ui_escape(unreachable_node.substr(0, 12)) + "</span></div>";
            out += "<div class=\"statgrid\"><div class=\"statcard\"><div class=\"schd\">"
                   "<span>Nodes</span><span class=\"scico\">" + web_ui_icon("nodes") +
                   "</span></div><div class=\"num\">" + std::to_string(node_count) +
                   "</div><div class=\"scft\"><span>" + std::to_string(live_count) +
                   " live</span><span class=\"pill " +
                   (unreachable_node.empty() ? "ok\">Healthy" : "warn\">Degraded") +
                   "</span></div></div>";
            out += "<div class=\"statcard\"><div class=\"schd\"><span>Projects</span>"
                   "<span class=\"scico\">" + web_ui_icon("projects") +
                   "</span></div><div class=\"num\">" +
                   std::to_string(context.node.project_names().size()) +
                   "</div><div class=\"scft\"><span>All Project data available</span>"
                   "<span class=\"pill ok\">Protected</span></div></div></div>";
            out += render_action_panel(
                context, view, "cluster", "Nodes",
                "membership, location, synchronization, reachability, and version",
                {"add_nodes", "detect_provisioners", "cloud_inspect"},
                ptable("SELECT * FROM system.nodes", "n"));
            out += web_ui_panel("Node metrics", "per-node resource + throughput metrics", ptable("SELECT * FROM system.node_metrics", "nm"));
            out += web_ui_panel("Deployments", "Pipelines placed onto targets, made runnable", ptable("SELECT * FROM system.deployments", "dep"));
        } else if (view == "projects") {
            out += web_ui_page_hero(
                "Projects", "Projects",
                "Intent, data, execution, outcomes, and history stay together.",
                "<span>Projects <b>" + std::to_string(context.node.project_names().size()) +
                    "</b></span><span>Workspace <b>this node</b></span>");
            out += render_projects(context, route);
        } else if (view == "data") {
            out += web_ui_page_hero(
                "Data library", "Data",
                "Find reusable sources and understand readiness, location, access, and "
                "Project relationships.",
                "<span>Scope <b>Workspace library</b></span>");
            out += render_search_panel(context, route);
            out += render_action_panel(
                context, view, "dataset", "Datasets",
                "tables loaded into the node \xC2\xB7 the source for search + training",
                {"add_dataset", "create_dataset_from_query", "promote_dataset"},
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
        } else if (view == "reports") {
            // Reports (#960 item 4): the IA and copy of the design's Reports screens
            // (oramasearch/orama-design, feat/reports-surface), rendering the ENGINE's
            // numbers through `get_run_report` rather than the design's fixtures.
            const std::string run_id = query_param(route, "run", "");
            if (run_id.empty()) {
                out += web_ui_page_hero(
                    "Workspace", "Reports",
                    "What a finished run found: how good the model is, what drives the "
                    "outcome, and which accounts are most at risk.", "");
                // One row per run that emitted a report output, newest first.
                nlohmann::json runs = nlohmann::json::array();
                for (const std::shared_ptr<zetta::pipeline::Run>& run : node.runs()) {
                    if (!run) continue;
                    bool has_report = false;
                    for (const zetta::pipeline::RunOutput& output : run->outputs()) {
                        if (output.kind == "report") has_report = true;
                    }
                    if (!has_report) continue;
                    runs.push_back({{"run_id", run->id()},
                                    {"graph", run->graph()},
                                    {"project", run->project()},
                                    {"status", to_string(run->status())},
                                    {"updated", run->updated()}});
                }
                std::sort(runs.begin(), runs.end(),
                          [](const nlohmann::json& left, const nlohmann::json& right) {
                              return left.value("updated", "") > right.value("updated", "");
                          });
                out += web_ui_reports_index(runs);
            } else {
                out += web_ui_page_hero(
                    "Workspace", "Report",
                    "Every number here was computed by the engine. Nothing on this page "
                    "was derived while rendering it.",
                    "<span>Run <b>" + web_ui_escape(run_id.substr(0, 8)) + "</b></span>",
                    "<a class=\"btn\" href=\"/?view=reports\">All reports</a>");
                const ApiResult report = capability_registry.call(
                    "get_run_report", {{"run_id", run_id}}, context);
                if (!report.ok) {
                    out += "<div class=\"empty\">" + web_ui_escape(report.error.message) +
                           "</div>";
                } else {
                    out += web_ui_run_report(report.value);
                }
            }
        } else if (view == "profile") {
            // The Workspace Profile (#886) — the destination "Discover
            // Opportunities" needed. Without it that card explained something you
            // could not begin, which is worse than not offering it.
            out += web_ui_page_hero(
                "Workspace", "Workspace Profile",
                "What you hold, what you connect, and what your work looks like. "
                "Auto-Suggest reads this to recommend what you could create.", "");
            ApiResult existing = capability_registry.call(
                "get_workspace_profile", nlohmann::json::object(), context);
            std::string body;
            if (existing.ok && existing.value.is_object() && !existing.value.empty()) {
                nlohmann::json rows = nlohmann::json::array();
                for (const auto& [key, value] : existing.value.items()) {
                    rows.push_back({{"field", key},
                                    {"value", value.is_string()
                                                  ? value.get<std::string>()
                                                  : value.dump()}});
                }
                body = web_ui_table(nlohmann::json{"field", "value"}, rows);
            } else {
                body =
                    "<div class=\"empty\">No Workspace Profile yet. Creating one "
                    "records your role, your daily work, and the sources you approve "
                    "&mdash; the input Auto-Suggest reads.</div>";
            }
            out += render_action_panel(
                context, view, "data", "Your profile",
                "role Â· daily work Â· approved sources",
                {"create_workspace_profile", "update_workspace_profile"}, body);
        } else if (view == "models") {
            out += web_ui_page_hero(
                "Developer", "Models",
                "Every Model is produced by a Project Run. Exposing a Model serves it to "
                "your Workspace so applications and Solutions can call it.",
                "<span>Context <b>Workspace</b></span>");
            out += render_action_panel(
                context, view, "models", "Trained Models",
                "select a Model to see the Run and evidence behind it \xC2\xB7 exposing and "
                "un-exposing are governed actions",
                {"expose_model", "unexpose_model"},
                ptable("SELECT * FROM system.models", "m"));
        } else if (view == "connections") {
            // Connections (July 24): the three connection paths — every card a
            // real, live destination — plus the saved source connections and
            // the safety contract.
            out += web_ui_page_hero(
                "Developer", "Connections",
                "Choose a connection path, understand its scope, and verify it without "
                "exposing credentials.",
                "<span>Workspace <b>this node</b></span><span>Credential health "
                "<b>Connected</b></span>");
            out += web_ui_panel(
                "Connection paths", "API, MCP, and CLI \xC2\xB7 workspace-scoped access",
                "<div class=\"worklist\">"
                "<a class=\"workrow\" href=\"/v1/openapi.json\"><span class=\"wstat\">"
                "<span class=\"pill ok\">Connected</span></span><span class=\"wname\">API"
                "<span class=\"actionsub\"> \xC2\xB7 workspace-scoped access for applications "
                "and services</span></span><span class=\"wnext\">View API setup</span>"
                "<span class=\"warr\">" + web_ui_icon("chevron") + "</span></a>"
                "<a class=\"workrow\" href=\"/mcp.md\"><span class=\"wstat\">"
                "<span class=\"pill ok\">Available</span></span><span class=\"wname\">MCP"
                "<span class=\"actionsub\"> \xC2\xB7 give approved coding agents access to "
                "this workspace</span></span><span class=\"wnext\">Configure MCP</span>"
                "<span class=\"warr\">" + web_ui_icon("chevron") + "</span></a>"
                "<a class=\"workrow\" href=\"/?view=shell\"><span class=\"wstat\">"
                "<span class=\"pill\">Console</span></span><span class=\"wname\">Shell"
                "<span class=\"actionsub\"> \xC2\xB7 inspect Projects, data, runs, and "
                "status from the governed console</span></span><span class=\"wnext\">Open "
                "Shell</span><span class=\"warr\">" + web_ui_icon("chevron") + "</span></a>"
                "</div>");
            out += render_action_panel(
                context, view, "token", "Source connections",
                "saved warehouse credentials, held on every node \xC2\xB7 the read surface "
                "never carries a token",
                {"create_connection", "delete_connection"},
                ptable("SELECT * FROM system.connections", "conn"));
            out += web_ui_panel(
                "Connection safety", "applies to API, MCP, CLI, desktop, and external agents",
                "<div class=\"legenddefs\">"
                "<div><b>Scoped access</b> every credential resolves to a user and a role; "
                "capability checks are default-deny.</div>"
                "<div><b>Visible activity</b> every action records its origin, actor, "
                "affected object, and result in the audit trail.</div>"
                "<div><b>Rotation and revocation</b> tokens can be revoked at any time; "
                "secrets are shown once and stored only as hashes.</div></div>");
        } else if (view == "agents") {
            out += web_ui_page_hero("Developer", "Agents",
                "Served agents: a model, a system prompt, and tools you can chat with. Creating, updating, and deleting are governed actions.", "");
            // The served agents (#849). No system.agents view yet, so the list is rendered
            // straight from the replicated records; create/update/delete are the capabilities.
            // Each row carries an inline, PRE-FILLED update_agent form (the system prompt +
            // name/description/tools are editable in place — the generic action form below is
            // blank, so this is how you edit an existing agent's prompt without retyping it).
            std::string table;
            if (context.node.replication() != nullptr) {
                std::string rows;
                for (const zetta::agent::AgentRecord& agent :
                     zetta::agent::AgentStore(*context.node.replication()).list()) {
                    std::string tools;
                    for (const std::string& tool : agent.tools) {
                        if (!tools.empty()) tools += ", ";
                        tools += tool;
                    }
                    // The tools field is a JSON array at the boundary (coerce_query_value
                    // JSON-parses it), so pre-fill it as JSON, not the comma view above.
                    const std::string tools_json =
                        web_ui_escape(nlohmann::json(agent.tools).dump());
                    // Empty fields aren't sent (web_form_input skips them) -> update_agent
                    // keeps the existing value, so a partial edit only changes what you touch.
                    const std::string edit_form =
                        "<details><summary class=\"btn small\">Edit</summary>"
                        "<form method=\"post\" action=\"/run\" class=\"agent-edit\">"
                        "<input type=\"hidden\" name=\"cap\" value=\"update_agent\">"
                        "<input type=\"hidden\" name=\"_return\" value=\"/?view=agents\">"
                        "<input type=\"hidden\" name=\"id\" value=\"" +
                        web_ui_escape(agent.id) +
                        "\">"
                        "<label>name</label><input name=\"name\" value=\"" +
                        web_ui_escape(agent.name) +
                        "\">"
                        "<label>description</label><input name=\"description\" value=\"" +
                        web_ui_escape(agent.description) +
                        "\">"
                        "<label>system prompt</label>"
                        "<textarea name=\"system_prompt\" rows=\"4\">" +
                        web_ui_escape(agent.system_prompt) +
                        "</textarea>"
                        "<label>tools (JSON array)</label><input name=\"tools\" value=\"" +
                        tools_json +
                        "\">"
                        "<div class=\"dlgact\"><button class=\"btn btn-primary\">Save</button>"
                        "</div></form></details>";
                    rows += "<tr><td>" +
                            web_ui_escape(agent.name.empty() ? agent.id : agent.name) +
                            "</td><td>" + web_ui_escape(agent.model) + "</td><td>" +
                            web_ui_escape(tools) + "</td><td>" + edit_form + "</td></tr>";
                }
                if (!rows.empty())
                    table = "<table class=\"grid\"><thead><tr><th>Name</th><th>Model</th>"
                            "<th>Tools</th><th></th></tr></thead><tbody>" +
                            rows + "</tbody></table>";
            }
            if (table.empty())
                table = "<p class=\"muted\">No agents yet \xE2\x80\x94 create one above, then talk "
                        "to it in the Chat view.</p>";
            out += render_action_panel(
                context, view, "models", "Agents",
                "served agents \xC2\xB7 a model + prompt + tools you can chat with \xC2\xB7 "
                "Edit a row to change its system prompt",
                {"create_agent", "update_agent", "delete_agent"}, table);
        } else if (view == "chat") {
            out += web_ui_page_hero("Developer", "Chat",
                "Talk to a served agent, or have it build a real Pipeline from your ask.", "");
            // Talk to a selected agent (#849), or have it build a real graph (#872). One
            // form, two submit buttons: the clicked button's name=cap picks the capability —
            // "Send" -> chat (reply only), "Build plan" -> agent_run (the agent recognizes the
            // ask, the engine compiles a graph from it and stores it proposed; nothing runs
            // until it is approved, #913). POSTed to /run WITHOUT a _return, so serve_web_run
            // renders the result inline. The turn blocks for seconds (model load +
            // generation) — fine for a request/response page.
            out += "<section class=\"panel\"><div class=\"phead\"><span class=\"ico\">" +
                   web_ui_icon("models") +
                   "</span><h2>Chat</h2><span class=\"psub\">talk to an agent \xC2\xB7 or have it "
                   "build a real pipeline from your ask</span></div>"
                   "<form method=\"post\" action=\"/run\">"
                   "<div class=\"fgrid\"><div><label>agent</label><select name=\"agent\">" +
                   agent_options(context) +
                   "</select></div><div class=\"full\"><label>message</label>"
                   "<textarea name=\"message\" rows=\"3\" placeholder=\"e.g. predict which "
                   "customers are about to churn, with calibrated scores + explanations\"></textarea></div></div>"
                   "<div class=\"dlgact\"><button class=\"btn\" name=\"cap\" value=\"chat\">Send</button>"
                   "<button class=\"btn btn-primary\" name=\"cap\" value=\"agent_run\">Build "
                   "plan</button></div>"
                   "</form></section>";
        } else if (view == "datagen") {
            out += web_ui_page_hero("Developer", "Data generators",
                "Generate and gate training datasets natively, grounded in the solutions plan.", "");
            // Native dataset generators (#873): the factory's generators + a Generate action.
            std::string rows;
            for (const std::shared_ptr<const zetta::dataset::DatasetGenerator>& generator :
                 zetta::dataset::DatasetGeneratorFactory::builtin().generators()) {
                rows += "<tr><td><code>" + web_ui_escape(generator->kind()) + "</code></td><td>" +
                        web_ui_escape(generator->title()) + "</td><td>" +
                        web_ui_escape(generator->description()) + "</td></tr>";
            }
            const std::string table =
                "<table class=\"grid\"><thead><tr><th>Kind</th><th>Name</th>"
                "<th>What it generates</th></tr></thead><tbody>" +
                rows + "</tbody></table>";
            out += render_action_panel(
                context, view, "datagen", "Data generators",
                "native training-dataset generators \xC2\xB7 grow + gate a corpus grounded in the "
                "solutions plan, inside Zetta (ADR-17) \xC2\xB7 Generate writes gated JSONL",
                {"generate_dataset"}, table);
        } else if (view == "pipeline") {
            out += web_ui_page_hero(
                "Operate", "Runs and activity",
                "Monitor work by Project first, then open infrastructure detail only when "
                "it explains an issue.",
                "<span>Scope <b>All Projects</b></span><span>Last refresh <b>just now</b></span>");
            // The live-execution banner (July 24): the newest running run, when
            // one exists — straight from system.runs.
            if (ApiResult running = capability_registry.call(
                    "sql",
                    nlohmann::json{{"statement",
                                    "SELECT run_id, status FROM system.runs WHERE status = "
                                    "'Running'"}},
                    context);
                running.ok && running.value.contains("rows") &&
                !running.value["rows"].empty()) {
                const auto& last_row = running.value["rows"].back();
                const std::string run_id =
                    last_row.is_object() && last_row.contains("run_id") &&
                            last_row["run_id"].is_string()
                        ? last_row["run_id"].get<std::string>()
                        : std::string();
                out += "<div class=\"bannerrow live\"><span class=\"bico\">" +
                       web_ui_icon("runs") +
                       "</span><span class=\"bbody\"><b>A run is executing now</b><span "
                       "class=\"mono\">run " +
                       web_ui_escape(run_id.substr(0, 8)) +
                       " \xC2\xB7 live in the Runs table below</span></span></div>";
            }
            out += render_action_bar(context, view, {"create_project", "capture_intent", "import_n8n",
                                                     "compile_intent", "compile_n8n", "compile_python",
                                                     "gate", "deploy", "run"});
            // The workspace-at-a-glance counts (moved off the Studio home —
            // Runs and activity is the operator's landing).
            if (ApiResult info = capability_registry.call("server_info",
                                                          nlohmann::json::object(), context);
                info.ok)
                out += web_ui_panel(
                    "Workspace at a glance", "",
                    web_ui_overview_cards(info.value) + "<div class=\"metaline mono\">" +
                        web_ui_escape(info.value.value("node_id", "")) + " \xC2\xB7 v" +
                        web_ui_escape(info.value.value("version", "")) + "</div>");
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
            out += web_ui_page_hero(
                "Operate", "Observability",
                "Every operation records its intent, plan, timing, and result \xE2\x80\x94 "
                "traces, spans, and crashes, threaded across surfaces and nodes.",
                "<span>Scope <b>this node</b></span>");
            out += web_ui_panel(
                "Traces", "one row per operation (an MCP/REST call, a run) \xC2\xB7 its intent, timing + result",
                ptable("SELECT * FROM system.traces", "tr"));
            out += web_ui_panel("Spans", "the timed steps inside a trace (parent \xE2\x86\x92 child), with durations",
                                ptable("SELECT * FROM system.spans", "sp"));
            out += web_ui_panel("Crashes", "captured crash reports, with stacktraces",
                                ptable("SELECT * FROM system.crashes", "cr"));
        } else if (view == "governance") {
            out += web_ui_page_hero(
                "Advanced", "Governance and audit",
                "Access is default-deny: every request resolves to a role, and every "
                "decision is recorded.",
                "<span>Roles <b>3 built-in</b></span><span>Access model "
                "<b>Default-deny</b></span><span>Audit <b>Append-only</b></span>");
            // Segmented tabs (July 24): Users and roles / Tokens / Consent / Audit.
            const std::string tab = query_param(route, "tab", "users");
            auto gtab = [&](const char* id, const char* label) {
                return std::string("<a class=\"") + (tab == id ? "on" : "") +
                       "\" href=\"/?view=governance&tab=" + id + "\">" + label + "</a>";
            };
            out += "<div class=\"segtabs\">" + gtab("users", "Users and roles") +
                   gtab("tokens", "Tokens") + gtab("consent", "Consent requests") +
                   gtab("audit", "Audit") + "</div>";
            // Approve / Deny live on each pending challenge (the Consent tab), not
            // as free-standing dialogs that ask for a challenge id (#999).
            out += render_action_bar(context, view, {"submit_feedback"});
            if (tab == "users") {
                out += web_ui_panel(
                    "Users",
                    "each user holds built-in roles \xC2\xB7 roles are seeded at node "
                    "bootstrap and replicated across the cluster",
                    ptable("SELECT * FROM system.users", "u"));
                out += web_ui_panel("Roles", "named permission bundles assigned to users",
                                    ptable("SELECT * FROM system.roles", "r"));
                out += web_ui_panel("Policies", "row / column access rules",
                                    ptable("SELECT * FROM system.policies", "pol"));
                out += "<div class=\"bannerrow warnb\"><span class=\"bico\">" +
                       web_ui_icon("governance") +
                       "</span><span class=\"bbody\"><b>Scoping is structural but "
                       "unused</b><span>Roles carry scope fields, but all built-ins are "
                       "unrestricted today. There are no per-Project or per-Workspace roles "
                       "yet.</span></span></div>";
            } else if (tab == "tokens") {
                out += web_ui_panel(
                    "Tokens",
                    "issued bearer tokens \xC2\xB7 secrets are shown once and stored only "
                    "as hashes; this view carries no secret material",
                    ptable("SELECT * FROM system.tokens", "tok"));
            } else if (tab == "consent") {
                out += render_consent_requests(context);
                out += web_ui_panel("Consent ledger",
                                    "every challenge on this node: pending, decided, executed",
                                    ptable("SELECT * FROM system.consent_requests", "cons"));
            } else {
                out += web_ui_panel(
                    "Audit", "the immutable, append-only record of every governed action",
                    ptable("SELECT * FROM system.audit", "aud"));
            }
        } else if (view == "shell") {
            out += web_ui_page_hero("Developer", "Shell",
                "SQL or Python against this node, governed: every statement is capability-scoped and audited.", "");
            out += web_ui_shell_repl();
        } else if (view == "notebooks") {
            out += web_ui_page_hero("Developer", "Notebooks",
                "SQL and Python cells, executed through the same governed console, saved in this browser.", "");
            out += web_ui_notebooks();
        } else {
            out += "<div class=\"empty\">Unknown view.</div>";
        }
        return out;
    }

    // GET / — the dashboard (or the login page when the session cookie is absent/invalid).
    boost::http::route_task serve_web_root(boost::http::route_params& route) {
        namespace http = boost::http;
        // The React Studio app replaces the server-rendered UI when a bundle
        // is configured (2026-07-24): / lands on the SPA entry.
        if (!options.webapp_dir.empty() &&
            std::filesystem::exists(std::filesystem::path(options.webapp_dir) / "index.html")) {
            route.status(http::status::found);
            route.res.set(http::field::location, "/studio/");
            stamp_response_headers(route, "");
            auto [redirect_error] = co_await route.send("");
            if (redirect_error) co_return http::route_error(redirect_error);
            co_return http::route_done;
        }
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        std::string user_name;
        if (!token.empty())
            co_await bridge.run_on_owner([&] {
                if (auto user = node.authenticate_token(token)) {
                    user_id = user->id();
                    user_name = user->name();
                }
            });
        std::string body;
        if (user_id.empty()) {
            body = web_ui_login("");
        } else {
            const std::string view = query_param(route, "view", "overview");
            std::string content;
            std::string meta;
            std::string assist_agent;
            std::string assist_agent_label;
            long long project_count = 0;
            co_await bridge.run_on_owner([&] {
                project_count = static_cast<long long>(node.project_names().size());
                // The Ask Orama rail talks to the first served agent (the base
                // agent when one is loaded); no agent → the rail's honest
                // empty state.
                if (node.replication() != nullptr) {
                    const auto agents =
                        zetta::agent::AgentStore(*node.replication()).list();
                    if (!agents.empty()) {
                        assist_agent = agents.front().id;
                        // Name the agent the rail will use, and its checkpoint. A node can
                        // serve several and this picks one silently (#981) — unnamed, a
                        // reply from an older agent reads as a model regression rather
                        // than as the wrong agent answering.
                        assist_agent_label = agents.front().name;
                        const std::string& served = agents.front().model;
                        if (const std::size_t slash = served.find_last_of('/');
                            slash != std::string::npos && slash + 1 < served.size()) {
                            const std::string checkpoint = served.substr(slash + 1);
                            if (checkpoint != assist_agent_label) {
                                assist_agent_label += " (" + checkpoint + ")";
                            }
                        }
                    }
                }
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
            body = web_ui_shell(view, user_name.empty() ? user_id : user_name, meta, content,
                                project_count, assist_agent, assist_agent_label);
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "text/html; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /login — exchange a bearer token for the HttpOnly session cookie.
    boost::http::route_task serve_web_login(boost::http::route_params& route) {
        namespace http = boost::http;
        StringBodySink sink;
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
        const std::string token = form_field(parse_form(sink.data), "token");
        bool ok = false;
        if (!token.empty())
            co_await bridge.run_on_owner([&] { if (node.authenticate_token(token)) ok = true; });
        if (!ok) {
            const std::string body = web_ui_login("Invalid or expired token.");
            route.status(http::status::unauthorized);
            route.res.set(http::field::content_type, "text/html; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
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

    // POST /session — the SPA's JSON login (#884): exchange a bearer token for
    // the same HttpOnly session cookie the form login sets, returning the
    // resolved identity as JSON. POST /session/logout clears it. The token
    // never lives in the page's JavaScript.
    boost::http::route_task serve_session_login(boost::http::route_params& route) {
        namespace http = boost::http;
        StringBodySink sink;
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
        std::string token;
        if (nlohmann::json input = nlohmann::json::parse(sink.data, nullptr, false);
            input.is_object() && input.contains("token") && input["token"].is_string())
            token = input["token"].get<std::string>();
        std::string user_id;
        std::string user_name;
        if (!token.empty())
            co_await bridge.run_on_owner([&] {
                if (auto user = node.authenticate_token(token)) {
                    user_id = user->id();
                    user_name = user->name();
                }
            });
        std::string body;
        if (user_id.empty()) {
            route.status(http::status::unauthorized);
            body = nlohmann::json{{"ok", false}, {"error", "invalid or expired token"}}.dump();
        } else {
            std::string cookie =
                "zetta_session=" + token + "; HttpOnly; SameSite=Strict; Path=/";
            if (request_is_https(route)) cookie += "; Secure";
            route.status(http::status::ok);
            route.res.set(http::field::set_cookie, cookie);
            body = nlohmann::json{{"ok", true},
                                  {"user", {{"id", user_id}, {"name", user_name}}}}
                       .dump();
        }
        route.res.set(http::field::content_type, "application/json");
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    boost::http::route_task serve_session_logout(boost::http::route_params& route) {
        namespace http = boost::http;
        route.status(http::status::ok);
        route.res.set(http::field::set_cookie,
                      "zetta_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        const std::string body = R"({"ok":true})";
        route.res.set(http::field::content_type, "application/json");
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
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
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
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
            // Same shell state as every page (badge + rail), so navigation
            // never differs on a result page. Node reads ride the owner
            // thread, like everything else.
            std::string assist_agent;
            long long project_count = 0;
            co_await bridge.run_on_owner([&] {
                project_count = static_cast<long long>(node.project_names().size());
                if (node.replication() != nullptr) {
                    const auto agents = zetta::agent::AgentStore(*node.replication()).list();
                    if (!agents.empty()) assist_agent = agents.front().id;
                }
            });
            body = web_ui_shell("console",
                                user_id.size() > 12 ? user_id.substr(0, 12) : user_id, "", content,
                                project_count, assist_agent);
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "text/html; charset=utf-8");
        route.res.set(http::field::cache_control, "no-store");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // POST /assist — the Ask Orama rail (#874): a cookie-authed JSON body
    // {agent, message} invoked through the same `chat` capability the MCP/REST
    // surfaces expose, audited as the signed-in user. The reply is the served
    // agent's; the turn blocks while the model loads + generates (the rail's
    // JS shows a thinking state — streaming is a later slice).
    boost::http::route_task serve_assist(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        StringBodySink sink;
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
        std::string json_body;
        if (user_id.empty()) {
            json_body = R"({"ok":false,"error":"unauthorized"})";
        } else {
            nlohmann::json input = nlohmann::json::parse(sink.data, nullptr, false);
            if (input.is_discarded() || !input.is_object()) {
                json_body = R"({"ok":false,"error":"malformed json"})";
            } else {
                co_await bridge.run_on_owner([&] {
                    ApiContext context{node, cluster, user_id,
                                       zetta::observability::make_root_trace(),
                                       &node.span_recorder(), &background};
                    context.acting_is_user = true;
                    ApiResult result = capability_registry.call("chat", input, context);
                    json_body = result.ok
                                    ? nlohmann::json{{"ok", true}, {"value", result.value}}.dump()
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
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
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
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
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

    // ----- the cookie blob routes: who may touch which large object (#1389) -----
    // These three routes are registered before the main gate (the web UI posts the
    // raw file body with its session cookie), so a resolved session is only who is
    // asking. Each route then checks what that user may do, the way the governed
    // serving path does (serve_blob), and audits the decision either way:
    //  - upload and delete change a database's large objects: sql:write on that
    //    database (a flat right, or a grant ON DATABASE <db>);
    //  - download reads one object: blob:read covering its id (a flat right, or a
    //    blob id/prefix grant), exactly what serve_blob requires.
    // An app credential is an ordinary token for a user with its own roles, so it
    // is held to the same rules.
    static zetta::governance::Resource blob_database(const std::string& db) {
        zetta::governance::Resource database;
        database.kind = zetta::governance::ResourceKind::Database;
        database.name = db;
        return database;
    }

    // Owner-thread only. Empty when `user_id` may write large objects in `db`,
    // else the refusal.
    std::string blob_write_refusal(const std::string& user_id, const std::string& db) const {
        const zetta::governance::Operation write{zetta::governance::CapabilityDomain::Data,
                                                 "write"};
        if (node.authorize_object(user_id, /*acting_is_user=*/true, write, blob_database(db))) {
            return {};
        }
        return "permission denied: no sql:write grant on database '" + db + "'";
    }

    // Owner-thread only. Empty when `user_id` may read the large object `id`, else
    // the refusal.
    std::string blob_read_refusal(const std::string& user_id, const std::string& id) const {
        if (node.authorize_blob(user_id, /*acting_is_user=*/true, id)) return {};
        return "permission denied: no blob:read grant for blob '" + id + "'";
    }

    // Owner-thread only. One audit event per blob route decision.
    void audit_blob_route(zetta::governance::AuditAction action, const std::string& user_id,
                          const std::string& verb, const std::string& target, bool allowed,
                          const std::string& detail) {
        zetta::governance::AuditEventValue event;
        event.action = action;
        event.actor = user_id;
        event.target = "web:blob_" + verb + ":" + target;
        event.allowed = allowed;
        event.detail = detail;
        node.record_audit_event(std::move(event));
    }

    // POST /blob?db=<database>&name=<filename> — store the raw request body as a large object
    // (file/blob) in the database's LargeObjectStore. Cookie-authed like the rest of the web UI;
    // the body IS the file bytes (the client reads the File and posts it raw — no multipart parser).
    // Needs sql:write on the database (#1389).
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
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
        std::string json_body;
        if (user_id.empty()) {
            json_body = R"({"error":"unauthorized"})";
        } else if (db.empty() || name.empty()) {
            json_body = R"({"error":"db and name query params are required"})";
        } else if (sink.data.empty()) {
            json_body = R"({"error":"empty file"})";
        } else {
            co_await bridge.run_on_owner([&] {
                const std::string refusal = blob_write_refusal(user_id, db);
                if (!refusal.empty()) {
                    audit_blob_route(zetta::governance::AuditAction::DataChanged, user_id,
                                     "upload", db + "/" + name, /*allowed=*/false, refusal);
                    json_body = nlohmann::json{{"error", refusal}}.dump();
                    return;
                }
                const std::shared_ptr<zetta::data::Database> database = node.find_database(db);
                if (!database) {
                    json_body = nlohmann::json{{"error", "no such database '" + db + "'"}}.dump();
                    return;
                }
                const std::optional<zetta::data::LargeObject> stored =
                    database->large_objects().import_bytes(name, sink.data);
                audit_blob_route(zetta::governance::AuditAction::DataChanged, user_id, "upload",
                                 db + "/" + (stored ? stored->id : name), stored.has_value(),
                                 stored ? "stored a large object" : "could not store the file");
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

    // POST /blob/delete?db=<database>&id=<object_id> — unlink a large object. Needs
    // sql:write on the database (#1389).
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
        // A body-stream failure is a real route error, not a silently
        // empty form: reading zero bytes because the peer went away must
        // not fall through and be handled as "no fields supplied" (#992).
        auto [read_error, bytes_read] =
            co_await boost::capy::push_to(route.req_body, sink);
        (void)bytes_read;
        if (read_error) co_return http::route_error(read_error);
        std::string json_body;
        if (user_id.empty()) {
            json_body = R"({"error":"unauthorized"})";
        } else {
            co_await bridge.run_on_owner([&] {
                const std::string refusal = blob_write_refusal(user_id, db);
                if (!refusal.empty()) {
                    audit_blob_route(zetta::governance::AuditAction::DataChanged, user_id,
                                     "delete", db + "/" + id, /*allowed=*/false, refusal);
                    json_body = nlohmann::json{{"error", refusal}}.dump();
                    return;
                }
                const std::shared_ptr<zetta::data::Database> database = node.find_database(db);
                if (!database) {
                    json_body = R"({"error":"no such database"})";
                    return;
                }
                const bool ok = database->large_objects().unlink(id);
                audit_blob_route(zetta::governance::AuditAction::DataChanged, user_id, "delete",
                                 db + "/" + id, ok,
                                 ok ? "unlinked a large object" : "no such large object");
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
    // Needs blob:read covering the id, as serve_blob does (#1389).
    boost::http::route_task serve_blob_download(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string token = cookie_value(route, "zetta_session");
        std::string user_id;
        if (!token.empty())
            co_await bridge.run_on_owner(
                [&] { if (auto user = node.authenticate_token(token)) user_id = user->id(); });
        const std::string db = query_param(route, "db", "");
        const std::string id = query_param(route, "id", "");
        std::string bytes, filename, error, refusal;
        if (!user_id.empty())
            co_await bridge.run_on_owner([&] {
                refusal = blob_read_refusal(user_id, id);
                if (!refusal.empty()) {
                    audit_blob_route(zetta::governance::AuditAction::DataAccess, user_id,
                                     "download", db + "/" + id, /*allowed=*/false, refusal);
                    return;
                }
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
                audit_blob_route(zetta::governance::AuditAction::DataAccess, user_id, "download",
                                 db + "/" + id, /*allowed=*/true, "served a large object");
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
        if (!refusal.empty()) {
            const std::string body = nlohmann::json{{"error", refusal}}.dump();
            route.status(http::status::forbidden);
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

    // GET /fonts/<name>.woff2 — the IBM Plex faces, embedded at build time and
    // served by the node itself (the self-contained page rule: no external
    // font requests, ever). Immutable-cacheable: the bytes only change with
    // the binary.
    boost::http::route_task serve_font(boost::http::route_params& route,
                                       const unsigned char* data, unsigned length) {
        namespace http = boost::http;
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "font/woff2");
        route.res.set(http::field::cache_control, "public, max-age=31536000, immutable");
        const std::string_view body(reinterpret_cast<const char*>(data), length);
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
        if (send_error) co_return http::route_error(send_error);
        co_return http::route_done;
    }

    // GET /app[/...] — the built Studio web app (the design repo's React
    // bundle, #874 architecture revision 2026-07-24): static files from
    // options.webapp_dir with an SPA fallback to index.html for client-side
    // routes. Public like /style.css — the bundle is the static shell; its
    // only data path is the gated /v1 surface. Small local reads, same
    // blocking pattern as the other asset routes.
    boost::http::route_task serve_webapp(boost::http::route_params& route,
                                         std::string rel_url) {
        namespace http = boost::http;
        std::string body;
        std::string served;
        if (!options.webapp_dir.empty()) {
            if (const auto rel = web_ui_webapp_path(rel_url)) {
                const std::filesystem::path root(options.webapp_dir);
                const auto read_file = [&](const std::filesystem::path& file) {
                    std::ifstream in(file, std::ios::binary);
                    if (!in) return false;
                    std::ostringstream buffer;
                    buffer << in.rdbuf();
                    body = std::move(buffer).str();
                    return true;
                };
                if (read_file(root / *rel)) {
                    served = *rel;
                } else if (rel->find('.') == std::string::npos &&
                           read_file(root / "index.html")) {
                    served = "index.html";  // the SPA fallback for client routes
                }
            }
        }
        if (served.empty()) {
            const std::string message =
                options.webapp_dir.empty()
                    ? "No web app bundle configured - start the node with --webapp-dir PATH."
                    : "Not found in the web app bundle.";
            route.status(http::status::not_found);
            route.res.set(http::field::content_type, "text/plain; charset=utf-8");
            stamp_response_headers(route, message);
            auto [send_error] = co_await route.send(message);
            if (send_error) co_return http::route_error(send_error);
            co_return http::route_done;
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, std::string(web_ui_content_type(served)));
        route.res.set(http::field::cache_control,
                      served == "index.html" ? "no-store"
                                             : "public, max-age=31536000, immutable");
        stamp_response_headers(route, body);
        auto [send_error] = co_await route.send(body);
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
    namespace corosio = boost::corosio;

    Impl& impl = *impl_;
    if (impl.is_running.load()) return true;

    http::router router;
    router.use([](http::route_params& route) -> http::route_task {
        // Workers retain response fields across requests and cached connections.
        // Only the current handler may establish a session or choose a redirect,
        // including on public GETs and CORS preflight responses.
        route.res.erase(http::field::set_cookie);
        route.res.erase(http::field::location);
        co_return http::route_next;
    });
    router.use(http::cors());
    // Browser POST routes always require origin binding, including login (which
    // has no existing session) and logout, before reading a body or changing state.
    auto browser_post = [&router, &impl](const char* path, auto handler) {
        router.add(http::method::post, path,
                   [&impl, handler](http::route_params& route) -> http::route_task {
                       if (!trusted_browser_origin(route))
                           co_return co_await refuse_browser_origin(route);
                       co_return co_await (impl.*handler)(route);
                   });
    };
    // GET / — the server-rendered web dashboard (#647, web_ui.cpp). PUBLIC route: it self-authenticates
    // from the HttpOnly `zetta_session` cookie and serves the login page when it is absent/invalid.
    // POST /login, /logout, /run are its companions — all registered BEFORE the auth gate (they do
    // their own cookie auth), so a browser navigation stays authenticated without a bearer header.
    router.add(http::method::get, "/", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_web_root(route);
    });
    browser_post("/login", &Impl::serve_web_login);
    browser_post("/session", &Impl::serve_session_login);
    browser_post("/session/logout", &Impl::serve_session_logout);
    browser_post("/logout", &Impl::serve_web_logout);
    browser_post("/run", &Impl::serve_web_run);
    browser_post("/assist", &Impl::serve_assist);
    browser_post("/shell", &Impl::serve_web_shell);
    browser_post("/graph-save", &Impl::serve_graph_save);
    browser_post("/blob", &Impl::serve_blob_upload);
    browser_post("/blob/delete", &Impl::serve_blob_delete);
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
    router.add(http::method::get, "/fonts/plex-sans-400.woff2",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_font(route, kPlexSansWoff400, kPlexSansWoff400_len);
               });
    router.add(http::method::get, "/fonts/plex-sans-500.woff2",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_font(route, kPlexSansWoff500, kPlexSansWoff500_len);
               });
    router.add(http::method::get, "/fonts/plex-sans-600.woff2",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_font(route, kPlexSansWoff600, kPlexSansWoff600_len);
               });
    router.add(http::method::get, "/fonts/plex-mono-400.woff2",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_font(route, kPlexMonoWoff400, kPlexMonoWoff400_len);
               });
    // The Studio web app bundle (the design repo's React build) REPLACES the
    // server-rendered UI when --webapp-dir is set (2026-07-24): / redirects to
    // /studio/, the SPA entry; the bundle's asset paths serve here, pre-gate,
    // exactly like /style.css. The SSR pages remain the no-bundle fallback.
    auto joined = [](http::route_params& route, std::string prefix) {
        for (const auto& [parameter, value] : route.params) {
            if (!prefix.empty()) prefix += '/';
            prefix += value;
        }
        return prefix;
    };
    router.add(http::method::get, "/studio", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_webapp(route, "");  // the SPA entry (client route)
    });
    router.add(http::method::get, "/studio/:p1", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_webapp(route, "");  // client subroutes → index.html
    });
    router.add(http::method::get, "/assets/:p1", [&impl, joined](http::route_params& route) -> http::route_task {
        return impl.serve_webapp(route, joined(route, "assets"));
    });
    router.add(http::method::get, "/assets/:p1/:p2", [&impl, joined](http::route_params& route) -> http::route_task {
        return impl.serve_webapp(route, joined(route, "assets"));
    });
    router.add(http::method::get, "/favicon.png", [&impl](http::route_params& route) -> http::route_task {
        return impl.serve_webapp(route, "favicon.png");
    });
    // The bundle's own fonts (Geist etc.) — the embedded Plex routes above
    // stay first; anything else under /fonts resolves from the bundle.
    router.add(http::method::get, "/fonts/:p1", [&impl, joined](http::route_params& route) -> http::route_task {
        return impl.serve_webapp(route, joined(route, "fonts"));
    });

    // Discovery docs are registered before the gate, like /mcp.md: llms.txt is meant to be
    // fetched credential-free (an agent reads it to learn how to authenticate). Each answers
    // per caller (#1357): an authenticated caller reads every capability; an anonymous one
    // the publicly callable set, which is empty, so it learns how to authenticate and no
    // more. LLM renderers take no Node state (#1402). The capability routes + /v1/specs
    // stay bearer-gated below.
    impl.openapi_body = openapi_document(impl.capability_registry).dump(2);
    impl.openapi_public_body =
        openapi_document(impl.capability_registry, publicly_callable()).dump(2);
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
    // GET /cli.md — public, like /mcp.md: the node's own command-line reference.
    // Public because you must be able to read how to run a node without already
    // holding a token from one.
    router.add(http::method::get, "/cli.md",
               [&impl](http::route_params& route) -> http::route_task {
                   std::string host;
                   if (route.req.count(http::field::host)) {
                       host = std::string(route.req.at(http::field::host));
                   }
                   if (host.empty()) {
                       host = impl.options.listen_address + ":" +
                              std::to_string(impl.options.port);
                   }
                   const std::string origin =
                       (request_is_https(route) ? "https://" : "http://") + host;
                   const std::string body = render_cli_md(origin);
                   route.status(http::status::ok);
                   route.res.set(http::field::content_type,
                                 "text/markdown; charset=utf-8");
                   stamp_response_headers(route, body);
                   auto [error] = co_await route.send(body);
                   if (error) co_return http::route_error(error);
                   co_return http::route_done;
               });

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

    // Root-level bundle files (brand marks, og images) and top-level client
    // routes — GET, single segment, registered after every named public route
    // so it shadows nothing. Deeper unknown paths still hit the gate (401).
    router.add(http::method::get, "/:file", [&impl](http::route_params& route) -> http::route_task {
        std::string file;
        for (const auto& [parameter, value] : route.params)
            if (parameter == "file") file = value;
        return impl.serve_webapp(route, std::move(file));
    });

    // The ONE auth gate (#745, epic #743): a single middleware at the router
    // filter that runs before EVERY gated route (cors handles the CORS preflight
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
        // Authorization: Bearer wins; else the HttpOnly zetta_session cookie
        // (its value IS a bearer token, validated identically) — so the Studio
        // web app authenticates its /v1 calls with the session cookie set by
        // POST /session, the token never touching the page's JavaScript.
        std::string bearer = bearer_of(route);
        if (bearer.empty()) {
            bearer = cookie_value(route, "zetta_session");
            // Check every cookie-authenticated API method: a GET capability can
            // have effects too (SQL and consuming reads). Explicit bearers retain
            // the programmatic contract and never fall back after auth failure.
            if (!bearer.empty() && !trusted_browser_origin(route, true))
                co_return co_await refuse_browser_origin(route);
        }
        RequestIdentity identity;
        bool authorized = false;

        // #1417: no credential is refused here, with no owner-loop hop. A
        // source that has spent its failed-authentication budget gets only
        // the bounded lookups (an issued bearer's one token, an already
        // verified bearer's digest), never a token-store scan, and its
        // failures answer 429.
        const std::string source = impl.request_source(route);
        if (bearer.empty()) co_return co_await impl.refuse_unauthenticated(route);
        const auto budget = impl.auth_limiter.admit(source);

        std::string resolved_user_id =
            co_await impl.resolve_credential(bearer, budget.admitted);

        if (!resolved_user_id.empty()) {
            authorized = true;
            identity.acting_key = std::move(resolved_user_id);
            identity.is_user = true;
        }

        if (!authorized) {
            impl.auth_limiter.record_failure(source);
            if (!budget.admitted) {
                co_return co_await impl.refuse_rate_limited(route, budget.retry_after_seconds);
            }
            co_return co_await impl.refuse_unauthenticated(route);
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
        Impl::RequestLifetime request_lifetime(impl.requests_in_flight);
        if (route.req.method() != http::method::post) {
            route.status(http::status::method_not_allowed);
            stamp_response_headers(route, "POST /mcp only");
            auto [send_error] = co_await route.send("POST /mcp only");
            (void)send_error;
            co_return http::route_done;
        }

        // Auth already ran at the router authorization filter above; read the identity it
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
                // Registered like a REST poll's, so stop() wakes it too.
                auto wait_event = impl.new_poll_event();
                ApiResult observed;
                bool client_gone = false;
                bool blocks = false;
                if (request.is_object() &&
                    request.value("method", std::string{}) == "tools/call" &&
                    request.contains("params") && request.at("params").is_object() &&
                    request.at("params").contains("name") &&
                    request.at("params").at("name").is_string()) {
                    const auto capability = impl.capability_registry.find(
                        request.at("params").at("name").get<std::string>());
                    blocks = capability && capability->blocks;
                }
                if (blocks) {
                    auto dispatch = [&] {
                        return impl.dispatch(
                            request, identity.acting_key, identity.is_user,
                            traceparent, wait_event, &observed);
                    };
                    response_body =
                        co_await zetta::runtime::offload_blocking(dispatch);
                    co_await impl.bridge.run_on_owner([] {});
                } else {
                    // Marshal the whole dispatch onto the loop that owns the
                    // Node; this coroutine suspends until drain() has run it.
                    co_await impl.bridge.run_on_owner([&] {
                        response_body = impl.dispatch(
                            request, identity.acting_key, identity.is_user,
                            traceparent, wait_event, &observed);
                    });
                }
                const auto awaited =
                    request.is_object() &&
                            request.value("method", std::string{}) == "tools/call" &&
                            request.contains("params") && request.at("params").is_object() &&
                            request.at("params").contains("name") &&
                            request.at("params").at("name").is_string()
                        ? impl.capability_registry.find(
                              request.at("params").at("name").get<std::string>())
                        : nullptr;
                if (awaited && awaited->awaitable_read && awaited->awaitable_read->retry_within) {
                    // Keep waiting until wait_for elapses (#1005), one governed
                    // call per wake, exactly as the REST rendering does.
                    const AwaitableRead& awaitable = *awaited->awaitable_read;
                    const auto deadline =
                        std::chrono::steady_clock::now() +
                        awaitable.wait_for(request.at("params").value(
                            "arguments", nlohmann::json::object()));
                    auto event = wait_event;
                    while (awaitable.should_wait(observed) && impl.is_running.load()) {
                        const auto remaining =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now());
                        if (remaining.count() <= 0) break;
                        if (!co_await impl.await_poll(
                                event, std::min(remaining, awaitable.retry_within(observed)),
                                route)) {
                            client_gone = true;
                            break;
                        }
                        impl.forget_poll_event(event);
                        event = impl.new_poll_event();
                        wait_event = event;
                        auto again = [&] {
                            return impl.dispatch(request, identity.acting_key, identity.is_user,
                                                 traceparent, event, &observed);
                        };
                        if (awaited->blocks) {
                            response_body = co_await zetta::runtime::offload_blocking(again);
                            co_await impl.bridge.run_on_owner([] {});
                        } else {
                            co_await impl.bridge.run_on_owner([&] { response_body = again(); });
                        }
                    }
                } else if (observed.ok && request.is_object() &&
                    request.value("method", std::string{}) == "tools/call") {
                    const auto& params = request.at("params");
                    const auto capability = impl.capability_registry.find(
                        params.at("name").get<std::string>());
                    if (capability && capability->awaitable_read &&
                        capability->awaitable_read->should_wait(observed)) {
                        const auto wait_for =
                            capability->awaitable_read->wait_for(params.value(
                                "arguments", nlohmann::json::object()));
                        if (wait_for.count() > 0 && impl.is_running.load() &&
                            !co_await impl.await_poll(wait_event, wait_for, route)) {
                            client_gone = true;
                        } else if (wait_for.count() > 0) {
                            co_await impl.bridge.run_on_owner([&] {
                                response_body = impl.dispatch(
                                    request, identity.acting_key,
                                    identity.is_user, traceparent);
                            });
                        }
                    }
                }
                impl.forget_poll_event(wait_event);
                if (client_gone) co_return http::route_close;  // nothing to answer (#1174)
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
        if (capability->post_alias && method == http::method::get) {
            router.add(http::method::post, rest_path(*capability),
                [&impl, capability](http::route_params& route) -> http::route_task {
                    return impl.serve_rest_capability(route, capability);
                });
        }
    }

    // The object capability's routes (#1005): the byte routes and the bucket
    // aliases, one handler that parses /v1/objects/<bucket>[/<key>|:<verb>].
    for (const http::method method : {http::method::get, http::method::put, http::method::patch,
                                      http::method::delete_, http::method::post}) {
        router.add(method, "/v1/objects/*rest",
                   [&impl](http::route_params& route) -> http::route_task {
                       return impl.serve_objects(route);
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
    // Published pipelines (zetta#1289): answered per request from the
    // replicated exposure, like the model and index routes.
    router.add(http::method::post, "/v1/pipelines/:name/run",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_pipeline_endpoint(route, /*status_read=*/false);
               });
    router.add(http::method::get, "/v1/pipelines/:name/runs/:run_id",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_pipeline_endpoint(route, /*status_read=*/true);
               });

    // LAST: answer whatever no route above matched (#1007) — an unknown path, or
    // a capability called with the wrong method — instead of leaving the client
    // waiting for a response that never comes. Must stay the final registration.
    router.use([&impl](http::route_params& route) -> http::route_task {
        return impl.serve_unrouted(route);
    });

    if (impl.options.max_connections == 0 || impl.options.listen_backlog <= 0) {
        impl.failure_reason = "max_connections and listen_backlog must be positive";
        return false;
    }
    impl.router.emplace(std::move(router));
    impl.parser_config = http::make_parser_config([] {
        http::parser_config config(/*server=*/true);
        config.body_limit = kMaxRequestBodyBytes;
        return config;
    }());
    impl.serializer_config = http::make_serializer_config(http::serializer_config());

    corosio::ipv4_address address(impl.options.listen_address);
    impl.acceptor.open();
    impl.acceptor.set_option(corosio::socket_option::reuse_address(true));
    auto bind_error = impl.acceptor.bind(corosio::endpoint(address, impl.options.port));
    if (!bind_error) bind_error = impl.acceptor.listen(impl.options.listen_backlog);
    if (bind_error) {
        impl.failure_reason = "bind failed: " + bind_error.message();
        impl.acceptor.close();
        return false;
    }
    impl.bound_port = impl.acceptor.local_endpoint().port();
    std::fprintf(stderr,
                 "zetta: HTTP listening on %s:%u (max_connections=%zu, "
                 "idle_connection_cache=%zu, listen_backlog=%d)\n",
                 impl.options.listen_address.c_str(), static_cast<unsigned>(impl.bound_port),
                 impl.options.max_connections, impl.options.idle_connection_cache,
                 impl.options.listen_backlog);
    impl.io_context.restart();
    // Keep run() alive while an offloaded request has no completion queued on
    // this executor. Shutdown releases this only after those request
    // coroutines have finished (#1077).
    impl.io_context.get_executor().on_work_started();
    impl.io_work_held.store(true);
    impl.is_running.store(true);
    runtime::run_async(impl.io_context.get_executor())(impl.accept_connections());
    impl.io_thread = std::thread([&impl] { impl.io_context.run(); });
    return true;
}

void ApiServer::stop_accepting() {
    Impl& impl = *impl_;
    if (!impl.is_running.exchange(false)) return;
    runtime::run_async(impl.io_context.get_executor())(
        impl.stop_accepting_connections());
}

void ApiServer::stop() {
    Impl& impl = *impl_;
    stop_accepting();
    if (!impl.io_thread.joinable()) return;

    runtime::run_async(impl.io_context.get_executor())(impl.stop_connections());
    // Closing admitted sockets requests cancellation but their session
    // coroutines still have to unwind on the live io_context. Keep both owner
    // queues moving while they do: an offloaded request can return through the
    // API bridge and its authorization can return through Node::run_on_owner.
    // Destroying the io_context with either hop suspended can deadlock while
    // its stop callback is being removed (#1077).
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        impl.bridge.drain();
        impl.node.drain_owner_work();
        impl.background.prune();
        if (impl.requests_in_flight.load() == 0 &&
            impl.bridge.pending() == 0) {
            break;
        }
        impl.bridge.wait_for_work(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    const bool drained = impl.requests_in_flight.load() == 0 &&
                         impl.bridge.pending() == 0;
    if (impl.io_work_held.exchange(false)) {
        impl.io_context.get_executor().on_work_finished();
    }
    if (!drained) {
        std::fprintf(stderr,
                     "zetta: API shutdown drain exceeded 5 seconds; "
                     "stopping the io context\n");
        impl.io_context.stop();
    }
    if (impl.io_thread.joinable()) impl.io_thread.join();
    impl.bridge.drain();
    impl.node.drain_owner_work();
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

void ApiServer::wake() { impl_->bridge.poke(); }
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
