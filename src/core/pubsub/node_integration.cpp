#include "zetta/pubsub/node_integration.hpp"

#include <chrono>

#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/node.hpp"

namespace zetta::pubsub {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

Operation operation_for(PubSubOperation operation) {
    switch (operation) {
        case PubSubOperation::Read: return {CapabilityDomain::Data, "topics_read"};
        case PubSubOperation::Publish: return {CapabilityDomain::Data, "pubsub_publish"};
        case PubSubOperation::Subscribe: return {CapabilityDomain::Data, "pubsub_subscribe"};
        case PubSubOperation::SendWildcard: return {CapabilityDomain::Data, "pubsub_send_wildcard"};
        case PubSubOperation::Create: return {CapabilityDomain::Orchestration, "pubsub_create"};
        case PubSubOperation::Manage: return {CapabilityDomain::Orchestration, "pubsub_manage"};
    }
    return {CapabilityDomain::Data, "pubsub_publish"};
}

class NodeAuthorizer final : public Authorizer {
public:
    explicit NodeAuthorizer(zetta::Node& node) : node_(node) {}

    bool authorize(const Address& principal, PubSubOperation operation, const Address& target) override {
        if (principal.kind == "service") return true;  // engine-internal
        if (principal.kind != "user") return false;    // deny by default
        // Users and roles are owner-thread state: publishers on other threads
        // (drains, long polls) marshal the decision onto the owner loop.
        bool allowed = false;
        node_.run_on_owner([&] {
            auto user = node_.find_user_by_id(principal.name);
            if (!user) user = node_.find_user(principal.name);
            if (!user) return;
            const auto op = operation_for(operation);
            if (target.kind != "topic" && target.kind != "inbox") {
                allowed = node_.authorize_user(user->id(), op).allowed;
                return;
            }
            // Global grants cover every topic; scoped-only grants cover their prefix.
            std::string name = target.name;
            if (!name.empty() && name.back() == '*') name.pop_back();
            // An inbox is its own resource (LSR-259, #1432): `inbox:<name>`, which no topic
            // prefix covers (an address name never holds ':'), so a grant on a topic namespace
            // never reaches an inbox in it. An inbox is granted explicitly, on `inbox:<name>`.
            if (target.kind == "inbox") name = "inbox:" + name;
            allowed = node_.authorize_object(user->id(), true, op, zetta::governance::Resource::topic(name));
        });
        return allowed;
    }

    DeliveryBounds bounds(const Address&, const Address&) override { return {}; }

    bool may_act_for(const Address&, const Address&) override { return true; }

private:
    zetta::Node& node_;
};

class TableDrain final : public InboxDrain {
public:
    explicit TableDrain(zetta::Node& node) : node_(node) {}

    DrainResult drain(const Address& target, const Event& event) override {
        const auto dot = target.name.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == target.name.size()) {
            return {.ok = false, .error = "a table address is table:<database>.<table>, not " + target.to_string()};
        }
        const std::string database_name = target.name.substr(0, dot);
        const std::string table_name = target.name.substr(dot + 1);
        auto row = nlohmann::json::parse(event.payload.bytes(), nullptr, false);
        if (!row.is_object()) {
            return {.ok = false, .error = "the payload is not a JSON object, so it cannot be a row of " +
                                          target.to_string()};
        }
        DrainResult result{.ok = false, .error = "not run"};
        node_.run_on_owner([&] {
            auto database = node_.find_database(database_name);
            if (!database) {
                result = {.ok = false, .error = "no database '" + database_name + "'"};
                return;
            }
            auto table = database->find_table(table_name);
            if (!table) {
                result = {.ok = false, .error = "no table '" + table_name + "' in '" + database_name + "'"};
                return;
            }
            const auto rows = table->add_row(row);
            result = {.ok = true, .result = nlohmann::json{{"inserted", 1}, {"rows", rows}}.dump()};
        });
        return result;
    }

private:
    zetta::Node& node_;
};

std::string iso8601(std::chrono::system_clock::time_point time) {
    const auto seconds = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

}  // namespace

std::shared_ptr<Authorizer> make_node_authorizer(zetta::Node& node) {
    return std::make_shared<NodeAuthorizer>(node);
}

void connect_audit_trail(PubSub& pubsub, zetta::Node& node) {
    pubsub.set_audit_sink([&node](const AuditRecord& record) {
        const auto action = zetta::governance::audit_action_from(record.action);
        if (!action) return;
        node.run_on_owner([&] {
            node.record_audit_event({.action = *action, .actor = record.principal.to_string(),
                .target = record.target.to_string(), .allowed = true, .detail = record.detail});
        });
    });
}

std::shared_ptr<InboxDrain> make_table_drain(zetta::Node& node) { return std::make_shared<TableDrain>(node); }

std::vector<nlohmann::json> pubsub_topic_rows(const PubSub& pubsub) {
    std::vector<nlohmann::json> rows;
    for (const auto& stats : pubsub.topic_stats()) {
        nlohmann::json row;
        row["topic"] = stats.address;
        row["lifecycle"] = stats.lifecycle == TopicLifecycle::Ephemeral ? "ephemeral" : "default";
        row["exposed"] = stats.exposed;
        row["subscribers"] = stats.subscribers;
        row["held_events"] = stats.held_events;
        row["memory_bytes"] = stats.memory_bytes;
        row["disk_bytes"] = stats.disk_bytes;
        row["spilling_since"] = stats.spilling_since ? iso8601(*stats.spilling_since) : std::string();
        row["slowest_subscriber"] = stats.slowest_subscriber;
        row["slowest_lag_events"] = stats.slowest_lag_events;
        row["next_sequence"] = stats.next_sequence;
        row["dispatch_mode"] = std::string(to_string(pubsub.dispatch_mode()));
        row["dispatch_threads"] = pubsub.dispatch_threads();
        row["dispatch_keys"] = pubsub.dispatch_keys();
        row["topics_per_dispatch_thread"] = pubsub.limits().topics_per_thread;
        row["dispatch_min_threads"] = pubsub.limits().dispatch_min_threads;
        row["dispatch_max_threads"] = pubsub.limits().dispatch_max_threads;
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace zetta::pubsub
