// Safe mode per database on the control surface (#1373, docs/api/safe-mode.md):
// read and change the level of a Zetta database or a saved source connection.
// The check itself runs in Registry::call, for every capability that carries a
// SafeModeDescriber (sql, create_dataset_from_query, list_source_tables).

#include <optional>
#include <string>
#include <utility>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/safe_mode.hpp"
#include "zetta/node.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::governance::SafeMode;
using zetta::governance::SafeModeTarget;

// The right to manage a database — CREATE / DROP it — is `sql:ddl` on that
// database (a flat grant or one ON <database>.*). Changing its level needs the
// same right.
const Operation kManageDatabase{CapabilityDomain::Data, "ddl"};
// The right to manage a connection — save or replace it — is
// `create_connection`. Changing its level needs the same right.
const Operation kManageConnection{CapabilityDomain::Orchestration, "create_connection"};
// Reading a connection's level needs the right to list connections.
const Operation kReadConnection{CapabilityDomain::Orchestration, "list_connections"};

nlohmann::json selector_properties() {
    return {{"database",
             {{"type", "string"},
              {"minLength", 1},
              {"description", "a Zetta database by name (give this OR connection)"}}},
            {"connection",
             {{"type", "string"},
              {"minLength", 1},
              {"description",
               "a saved source connection by name (give this OR database)"}}}};
}

nlohmann::json mode_property() {
    return {{"type", "string"},
            {"enum", {"off", "warn_writes", "warn_all"}},
            {"description",
             "off: every statement runs; warn_writes: a statement that writes "
             "answers safe_mode_warning first; warn_all: every statement does"}};
}

const nlohmann::json& set_safe_mode_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json properties = selector_properties();
        properties["mode"] = mode_property();
        return nlohmann::json{{"type", "object"},
                              {"required", {"mode"}},
                              {"additionalProperties", false},
                              {"properties", std::move(properties)}};
    }();
    return schema;
}

const nlohmann::json& get_safe_mode_schema() {
    static const nlohmann::json schema = {{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", selector_properties()}};
    return schema;
}

const nlohmann::json& safe_mode_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json properties = selector_properties();
        properties["mode"] = mode_property();
        properties["is_default"] = {
            {"type", "boolean"},
            {"description",
             "true when no level was set and the kind's default applies "
             "(warn_writes for a connection, off for a database)"}};
        properties["previous"] = {
            {"type", "string"},
            {"enum", {"off", "warn_writes", "warn_all"}},
            {"description", "set_safe_mode only: the level before this change"}};
        return nlohmann::json{{"type", "object"},
                              {"required", {"mode", "is_default"}},
                              {"additionalProperties", false},
                              {"properties", std::move(properties)}};
    }();
    return schema;
}

struct Selector {
    SafeModeTarget target = SafeModeTarget::Database;
    std::string name;
};

// Exactly one of `database` / `connection`, naming something that exists. The
// schema cannot say "exactly one" without oneOf (which the query-string REST
// coercion does not model), so it is checked here.
std::optional<Selector> resolve_selector(const nlohmann::json& input,
                                         ApiContext& context, ApiResult& refusal) {
    const bool has_database = input.contains("database");
    const bool has_connection = input.contains("connection");
    if (has_database == has_connection) {
        refusal = ApiResult::failure(
            "invalid_input",
            has_database ? "give either 'database' or 'connection', not both"
                         : "give either a 'database' or a 'connection' name");
        return std::nullopt;
    }
    if (has_connection) {
        const std::string name = input.at("connection").get<std::string>();
        if (!zetta::connect::find_connection(context.node, name)) {
            refusal = ApiResult::failure(
                "not_found",
                "no saved connection named '" + name + "' (list_connections)",
                "/connection");
            return std::nullopt;
        }
        return Selector{SafeModeTarget::Connection, name};
    }
    const std::string name = input.at("database").get<std::string>();
    if (name == "system" || name == zetta::connect::kConnectionDatabase ||
        name == zetta::governance::kSafeModeDatabase) {
        refusal = ApiResult::failure(
            "invalid_input",
            "'" + name + "' is a reserved database; SQL cannot write to it, so it "
            "has no safe mode level",
            "/database");
        return std::nullopt;
    }
    if (!context.node.find_database(name)) {
        refusal = ApiResult::failure("not_found", "no database named '" + name + "'",
                                     "/database");
        return std::nullopt;
    }
    return Selector{SafeModeTarget::Database, name};
}

// Whether the caller holds `operation` — on `database` when one is named, for a
// user whose grant may be scoped ON <database>.*. The trusted local session
// (an empty acting key, which no network surface produces) holds every right.
bool holds(ApiContext& context, const Operation& operation,
           const std::string& database) {
    if (context.acting_key.empty()) return true;
    if (!context.acting_is_user) {
        return static_cast<bool>(context.node.authorize(context.acting_key, operation));
    }
    if (!database.empty()) {
        return context.node.authorize_object(
            context.acting_key, /*acting_is_user=*/true, operation,
            zetta::governance::Resource{zetta::governance::ResourceKind::Database,
                                        database});
    }
    return static_cast<bool>(context.node.authorize_user(context.acting_key, operation));
}

nlohmann::json level_json(const Selector& selector, SafeMode mode, bool is_default) {
    nlohmann::json value;
    value[std::string(zetta::governance::to_string(selector.target))] = selector.name;
    value["mode"] = std::string(zetta::governance::to_string(mode));
    value["is_default"] = is_default;
    return value;
}

ApiResult handle_get_safe_mode(const nlohmann::json& input, ApiContext& context) {
    ApiResult refusal;
    const std::optional<Selector> selector = resolve_selector(input, context, refusal);
    if (!selector) return refusal;
    if (selector->target == SafeModeTarget::Connection &&
        !holds(context, kReadConnection, {})) {
        return ApiResult::failure(
            "unauthorized",
            "reading a connection's safe mode needs the list_connections right");
    }
    const std::optional<SafeMode> stored =
        zetta::governance::stored_safe_mode(context.node, selector->target, selector->name);
    return ApiResult::success(level_json(
        *selector, stored.value_or(zetta::governance::default_safe_mode(selector->target)),
        !stored.has_value()));
}

ApiResult handle_set_safe_mode(const nlohmann::json& input, ApiContext& context) {
    ApiResult refusal;
    const std::optional<Selector> selector = resolve_selector(input, context, refusal);
    if (!selector) return refusal;
    if (selector->target == SafeModeTarget::Connection
            ? !holds(context, kManageConnection, {})
            : !holds(context, kManageDatabase, selector->name)) {
        return ApiResult::failure(
            "unauthorized",
            selector->target == SafeModeTarget::Connection
                ? "changing a connection's safe mode needs the create_connection "
                  "right (the right to manage that connection)"
                : "changing a database's safe mode needs sql:ddl on database '" +
                      selector->name + "' (the right to manage that database)");
    }
    const std::optional<SafeMode> mode =
        zetta::governance::parse_safe_mode(input.at("mode").get<std::string>());
    if (!mode) {  // the schema's enum already refuses anything else
        return ApiResult::failure("invalid_input", "unknown safe mode", "/mode");
    }
    const SafeMode previous = zetta::governance::effective_safe_mode(
        context.node, selector->target, selector->name);
    if (const std::string failed = zetta::governance::store_safe_mode(
            context.node, selector->target, selector->name, *mode, context.acting_key);
        !failed.empty()) {
        return ApiResult::failure("engine_error", failed);
    }
    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::SafeModeChanged;
    event.actor = context.acting_key.empty() ? "local" : context.acting_key;
    event.target =
        std::string(zetta::governance::to_string(selector->target)) + ":" + selector->name;
    event.allowed = true;
    event.detail = std::string(zetta::governance::to_string(previous)) + " -> " +
                   std::string(zetta::governance::to_string(*mode));
    context.node.record_audit_event(std::move(event));

    nlohmann::json value = level_json(*selector, *mode, /*is_default=*/false);
    value["previous"] = std::string(zetta::governance::to_string(previous));
    return ApiResult::success(std::move(value));
}

}  // namespace

void register_safe_mode_capabilities(Registry& registry) {
    registry.add(Capability{
        "set_safe_mode",
        "Set a database's safe mode",
        "Set the safe mode level of one Zetta database or saved source connection: "
        "off (every statement runs), warn_writes (a statement that writes answers "
        "safe_mode_warning with the exact statement first) or warn_all (every "
        "statement does). The caller confirms a warning by repeating the call with "
        "its confirmation. Needs the right to manage what it names: sql:ddl on the "
        "database, or create_connection for a connection. The level replicates to "
        "every node and is audited as safe_mode_changed.",
        set_safe_mode_schema(),
        safe_mode_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_set_safe_mode});

    registry.add(Capability{
        "get_safe_mode",
        "Get a database's safe mode",
        "Read the safe mode level of one Zetta database or saved source connection, "
        "and whether it is the default (warn_writes for a connection, off for a "
        "database). Reading a connection's level needs list_connections. No side "
        "effects.",
        get_safe_mode_schema(),
        safe_mode_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_safe_mode});
}

}  // namespace zetta::api
