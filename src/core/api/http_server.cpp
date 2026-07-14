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
#include "zetta/api/serving.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/api/web_ui.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
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

// A query-string parameter, or `fallback` when absent.
std::string query_param(const boost::http::route_params& route, std::string_view name,
                        std::string_view fallback) {
    for (const auto& parameter : route.url.params())
        if (parameter.key == name) return std::string(parameter.value);
    return std::string(fallback);
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
    std::string render_capability_form(const Capability& capability) {
        const bool write = capability.kind != CapabilityKind::Read;
        std::string fields;
        int field_count = 0;
        const auto& schema = capability.input_schema;
        if (schema.is_object() && schema.contains("properties") && schema["properties"].is_object()) {
            const nlohmann::json required = schema.value("required", nlohmann::json::array());
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
                bool is_required = false;
                for (const auto& item : required)
                    if (item.is_string() && item.get<std::string>() == it.key()) is_required = true;
                const std::string type = it.value().value("type", "string");
                const std::string description = it.value().value("description", "");
                const bool full = it.key() == "path" || it.key() == "statement" ||
                                  it.key() == "source" || it.key() == "index" ||
                                  it.key() == "description" || description.size() > 48;
                fields += std::string("<div class=\"") + (full ? "full" : "") + "\"><label>" +
                          web_ui_escape(it.key()) +
                          (is_required ? "<span class=\"req\">*</span>" : "") + "</label>";
                fields += "<input name=\"" + web_ui_escape(it.key()) + "\" placeholder=\"" +
                          web_ui_escape(type) +
                          (description.empty() ? "" : " \xC2\xB7 " + web_ui_escape(description)) +
                          "\"></div>";
                ++field_count;
            }
        }
        std::string out = "<details class=\"action\"><summary><span class=\"aico\">" +
                          web_ui_icon(write ? "bolt" : "runs") + "</span><span class=\"an\">" +
                          web_ui_escape(capability.name) + "</span><span class=\"ad\">" +
                          web_ui_escape(capability.description) + "</span><span class=\"chev\">" +
                          web_ui_icon("chevron") + "</span></summary><div class=\"abody\">";
        out += "<form method=\"post\" action=\"/run\"><input type=\"hidden\" name=\"cap\" value=\"" +
               web_ui_escape(capability.name) + "\">";
        if (field_count) out += "<div class=\"fgrid\">" + fields + "</div>";
        out += "<div style=\"margin-top:14px\"><button class=\"btn";
        out += write ? " btn-primary" : "";
        out += "\">";
        if (write) out += web_ui_icon("play");
        out += "Run ";
        out += to_string(capability.kind);
        out += "</button></div></form></div></details>";
        return out;
    }

    std::string render_capabilities(std::initializer_list<std::string_view> names) {
        std::string items;
        for (std::string_view name : names)
            if (auto capability = capability_registry.find(std::string(name)))
                items += render_capability_form(*capability);
        if (items.empty()) return "";
        return web_ui_panel("Actions", "run a capability \xC2\xB7 forms expand on click", items);
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

        return web_ui_panel("Explorer", "databases \xC2\xB7 tables \xC2\xB7 rows",
                            "<div class=\"explorer\">" + dbcol + tcol + rcol + "</div>");
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
            out += web_ui_panel("Nodes", "", ptable("SELECT * FROM system.nodes", "n"));
            out += web_ui_panel("Node metrics", "", ptable("SELECT * FROM system.node_metrics", "nm"));
            out += web_ui_panel("Deployments", "", ptable("SELECT * FROM system.deployments", "dep"));
            out += render_capabilities({"add_nodes", "detect_provisioners", "cloud_inspect"});
        } else if (view == "data") {
            out += render_data_explorer(context, route);
            out += web_ui_panel("Indexes", "", ptable("SELECT * FROM system.indexes", "idx"));
            out += render_capabilities({"add_dataset", "expose_index", "unexpose_index"});
        } else if (view == "models") {
            out += web_ui_panel("Models", "", ptable("SELECT * FROM system.models", "m"));
            out += render_capabilities({"expose_model", "unexpose_model"});
        } else if (view == "pipeline") {
            out += web_ui_panel("Runs", "", ptable("SELECT * FROM system.runs", "runs"));
            out += web_ui_panel("Jobs", "", ptable("SELECT * FROM system.jobs", "jobs"));
            out += render_capabilities({"create_project", "capture_intent", "import_n8n",
                                        "compile_intent", "compile_n8n", "compile_python", "gate",
                                        "deploy", "run"});
        } else if (view == "observability") {
            out += web_ui_panel("Traces", "", ptable("SELECT * FROM system.traces", "tr"));
            out += web_ui_panel("Spans", "", ptable("SELECT * FROM system.spans", "sp"));
            out += web_ui_panel("Crashes", "", ptable("SELECT * FROM system.crashes", "cr"));
        } else if (view == "governance") {
            out += web_ui_panel("Users", "", ptable("SELECT * FROM system.users", "u"));
            out += web_ui_panel("Roles", "", ptable("SELECT * FROM system.roles", "r"));
            out += web_ui_panel("Policies", "", ptable("SELECT * FROM system.policies", "pol"));
            out += web_ui_panel("Tokens", "", ptable("SELECT * FROM system.tokens", "tok"));
            out += web_ui_panel("Consent requests", "",
                                ptable("SELECT * FROM system.consent_requests", "cons"));
            out += web_ui_panel("Audit", "", ptable("SELECT * FROM system.audit", "aud"));
            out += render_capabilities({"approve_consent", "deny_consent", "submit_feedback"});
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
            std::string content;
            co_await bridge.run_on_owner([&] {
                auto capability = capability_registry.find(cap_name);
                if (!capability) {
                    content = web_ui_panel("Unknown capability", "",
                                           "<div class=\"err-box\">" + web_ui_escape(cap_name) +
                                               "</div>");
                    return;
                }
                std::vector<std::pair<std::string, std::string>> query;
                if (capability->input_schema.is_object() &&
                    capability->input_schema.contains("properties"))
                    for (auto it = capability->input_schema["properties"].begin();
                         it != capability->input_schema["properties"].end(); ++it) {
                        const std::string value = form_field(fields, it.key());
                        if (!value.empty()) query.emplace_back(it.key(), value);
                    }
                const nlohmann::json input = rest_input_from_query(*capability, query);
                ApiContext context{node, cluster, user_id,
                                   zetta::observability::make_root_trace(), &node.span_recorder(),
                                   &background};
                context.acting_is_user = true;
                ApiResult result = capability_registry.call(cap_name, input, context);
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
