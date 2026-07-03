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

#include "zetta/api/background.hpp"
#include "zetta/api/bridge.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/llms.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/api/specs.hpp"
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

// The bearer presented on a request, or empty ("Bearer <token>" only).
std::string bearer_of(const boost::http::route_params& route) {
    if (route.req.count(boost::http::field::authorization) == 0) return {};
    std::string_view header_value =
        route.req.at(boost::http::field::authorization);
    if (!header_value.starts_with("Bearer ")) return {};
    return std::string(header_value.substr(7));
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
    // (/llms-full.txt), or a topic name (/llms/<topic>.txt). Same bearer
    // policy as /mcp; rendering touches the Node, so it is marshaled onto the
    // owning loop via the bridge, wrapped in a span (every operation is
    // observable). Shared by the three GET routes registered in start().
    boost::http::route_task serve_llms(boost::http::route_params& route,
                                       std::string page) {
        namespace http = boost::http;

        const std::string bearer = bearer_of(route);
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

    // The serialized OpenAPI document — generated from the registry once at
    // start() (schema-as-code: the same generator output is drift-checked
    // into schemas/openapi.json) and served at GET /v1/openapi.json.
    std::string openapi_body;

    // Serve one REST rendering of a capability (#618, docs/api/rest-openapi.md):
    // GET for reads (input in the query string, coerced by the input schema),
    // POST for actions/jobs (input as the JSON body). Same bearer policy as
    // /mcp, and the call itself — validate, authorize + audit, handle, span —
    // is Registry::call on the owning loop, exactly the MCP path.
    boost::http::route_task serve_rest_capability(
        boost::http::route_params& route,
        std::shared_ptr<const Capability> capability) {
        namespace http = boost::http;

        std::string response_body;
        http::status status = http::status::ok;

        const std::string bearer = bearer_of(route);
        if (!options.bearer_token.empty() && bearer != options.bearer_token) {
            status = http::status::unauthorized;
            response_body = transport_error_body(
                "unauthorized", "missing or invalid bearer token");
        }

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
                ApiContext context{node, cluster, bearer, trace,
                                   &node.span_recorder(), &background};
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
    // never from disk. Same bearer policy as /mcp; rendering only reads the
    // (start-frozen) registry and static accessors, but is marshaled onto the
    // owning loop so the span lands in the node's ring (every operation is
    // observable). `name` empty = the index.
    boost::http::route_task serve_specs(boost::http::route_params& route,
                                        std::string name) {
        namespace http = boost::http;

        const std::string bearer = bearer_of(route);
        if (!options.bearer_token.empty() && bearer != options.bearer_token) {
            route.status(http::status::unauthorized);
            const std::string body = transport_error_body(
                "unauthorized", "missing or invalid bearer token");
            route.res.set(http::field::content_type, "application/json");
            stamp_response_headers(route, body);
            auto [send_error] = co_await route.send(body);
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

    // GET /v1/openapi.json — the OpenAPI rendering, same bearer policy.
    boost::http::route_task serve_openapi(boost::http::route_params& route) {
        namespace http = boost::http;
        const std::string bearer = bearer_of(route);
        if (!options.bearer_token.empty() && bearer != options.bearer_token) {
            route.status(http::status::unauthorized);
            const std::string body = transport_error_body(
                "unauthorized", "missing or invalid bearer token");
            route.res.set(http::field::content_type, "application/json");
            stamp_response_headers(route, body);
            auto [send_error] = co_await route.send(body);
            (void)send_error;
            co_return http::route_done;
        }
        route.status(http::status::ok);
        route.res.set(http::field::content_type, "application/json");
        stamp_response_headers(route, openapi_body);
        auto [send_error] = co_await route.send(openapi_body);
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
        const std::string bearer = bearer_of(route);
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

    // The REST rendering (#618, docs/api/rest-openapi.md): every registry
    // entry mechanically becomes GET /v1/<name> (reads) or POST /v1/<name>
    // (actions/jobs), and the OpenAPI document generated from the same
    // schemas is served at GET /v1/openapi.json — one registry, two wire
    // renderings, zero drift. Generated here, after any embedding-added
    // entries (the registry is populated before start() and then only read).
    impl.openapi_body = openapi_document(impl.capability_registry).dump(2);
    router.add(http::method::get, "/v1/openapi.json",
               [&impl](http::route_params& route) -> http::route_task {
                   return impl.serve_openapi(route);
               });
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
