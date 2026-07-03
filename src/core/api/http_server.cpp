// The ONE Beast2 adapter TU (docs/api/registry-design.md → Placement + build;
// docs/research/beast2-http.md). No beast2 / cppalliance-http / Boost.JSON
// name may appear outside this file; Boost.JSON <-> nlohmann conversion
// happens exactly once, here, at the wire boundary.
#include "zetta/api/server.hpp"

#include <atomic>
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

#include "zetta/api/bridge.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/llms.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
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

}  // namespace

struct ApiServer::Impl {
    Node& node;
    const zetta::cluster::Cluster* cluster;
    ServerOptions options;

    Registry capability_registry;
    MainThreadBridge bridge;

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
                         const std::string& acting_key,
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
                tools.push_back({{"name", capability->name},
                                 {"title", capability->title},
                                 {"description", capability->description},
                                 {"inputSchema", capability->input_schema}});
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
                                   &node.span_recorder()};
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
    // (/llms-full.txt), or a topic name (/llms/<topic>.txt). Same bearer
    // policy as /mcp; rendering touches the Node, so it is marshaled onto the
    // owning loop via the bridge, wrapped in a span (every operation is
    // observable). Shared by the three GET routes registered in start().
    boost::http::route_task serve_llms(boost::http::route_params& route,
                                       std::string page) {
        namespace http = boost::http;

        std::string bearer;
        if (route.req.count(http::field::authorization) != 0) {
            std::string_view header_value =
                route.req.at(http::field::authorization);
            if (header_value.starts_with("Bearer ")) {
                bearer = std::string(header_value.substr(7));
            }
        }
        if (!options.bearer_token.empty() && bearer != options.bearer_token) {
            route.status(http::status::unauthorized);
            stamp_response_headers(route, "missing or invalid bearer token");
            auto [send_error] =
                co_await route.send("missing or invalid bearer token");
            (void)send_error;
            co_return http::route_done;
        }

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
    router.use("/mcp", [&impl](http::route_params& route) -> http::route_task {
        if (route.req.method() != http::method::post) {
            route.status(http::status::method_not_allowed);
            stamp_response_headers(route, "POST /mcp only");
            auto [send_error] = co_await route.send("POST /mcp only");
            (void)send_error;
            co_return http::route_done;
        }

        // Bearer auth: with a configured token the header must match exactly;
        // the token doubles as the acting AccessKey id. Without one, the
        // acting key is whatever bearer the caller presented (may be empty =
        // the audited unauthenticated-local session on the loopback default).
        std::string bearer;
        if (route.req.count(http::field::authorization) != 0) {
            std::string_view header_value =
                route.req.at(http::field::authorization);
            if (header_value.starts_with("Bearer ")) {
                bearer = std::string(header_value.substr(7));
            }
        }
        if (!impl.options.bearer_token.empty() &&
            bearer != impl.options.bearer_token) {
            route.status(http::status::unauthorized);
            stamp_response_headers(
                route, "{\"error\":\"missing or invalid bearer token\"}");
            auto [send_error] = co_await route.send(
                "{\"error\":\"missing or invalid bearer token\"}");
            (void)send_error;
            co_return http::route_done;
        }

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
                    response_body =
                        impl.dispatch(request, bearer, traceparent);
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

    // The llms.txt progressive-disclosure surface (#645,
    // docs/api/llms-endpoints.md): plain GET pages an agent with no MCP
    // client can orient from, generated live from the registry + node state
    // (llms.cpp). GET-only by registration; everything else is shared with
    // /mcp via Impl::serve_llms.
    router.add(http::method::get, "/llms.txt",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_llms(route, "index");
               });
    router.add(http::method::get, "/llms-full.txt",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_llms(route, "full");
               });
    router.add(http::method::get, "/llms/:topic",
               [&impl](http::route_params& route) -> http::route_task {
                   std::string topic;
                   for (const auto& [parameter, value] : route.params) {
                       if (parameter == "topic") topic = value;
                   }
                   // /llms/<topic>.txt is canonical; the bare name works too.
                   if (topic.ends_with(".txt")) {
                       topic.resize(topic.size() - 4);
                   }
                   return impl.serve_llms(route, topic);
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
void ApiServer::drain() { impl_->bridge.drain(); }
Registry& ApiServer::registry() { return impl_->capability_registry; }
zetta::observability::SpanRecorder& ApiServer::recorder() {
    return impl_->node.span_recorder();
}
void ApiServer::set_activity_hook(ActivityHook hook) {
    impl_->capability_registry.set_activity_hook(std::move(hook));
}

}  // namespace zetta::api
