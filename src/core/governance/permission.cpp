#include "zetta/governance/permission.hpp"

#include <array>

namespace zetta::governance {
namespace {

// One entry of the permission vocabulary: the atom's name and the operations it
// expands to. THE single source of truth for the mapping (see permission.hpp).
struct PermissionEntry {
    std::string_view name;
    std::vector<Operation> operations;
};

// The canonical vocabulary, in privilege-ish order (read → write → manage). The
// list, the mapping, and the built-in role sets below all derive from this — a
// new atom is one entry here plus (optionally) membership in a built-in role.
const std::vector<PermissionEntry>& vocabulary() {
    static const std::vector<PermissionEntry> entries = {
        {"sql:read", {{CapabilityDomain::Data, "read"}}},
        {"sql:write", {{CapabilityDomain::Data, "write"}}},
        {"sql:ddl", {{CapabilityDomain::Data, "ddl"}}},
        {"mcp:call", {{CapabilityDomain::Api, "query"}}},
        {"blob:read", {{CapabilityDomain::Data, "blob_read"}}},
        {"serving:read",
         {{CapabilityDomain::Data, "predict"}, {CapabilityDomain::Data, "search"}}},
        {"users:manage", {{CapabilityDomain::Api, "manage_users"}}},
        {"roles:manage", {{CapabilityDomain::Api, "manage_roles"}}},
        {"tokens:manage", {{CapabilityDomain::Api, "manage_tokens"}}},
        {"cluster:admin", {{CapabilityDomain::Api, "cluster_admin"}}},
        // Pub/sub (#1029): publish and subscribe are data rights, creating and
        // managing topics orchestration; sending to a wildcard is its own right.
        {"pubsub:publish", {{CapabilityDomain::Data, "pubsub_publish"}}},
        {"pubsub:subscribe", {{CapabilityDomain::Data, "pubsub_subscribe"}}},
        {"pubsub:send_wildcard", {{CapabilityDomain::Data, "pubsub_send_wildcard"}}},
        {"pubsub:create", {{CapabilityDomain::Orchestration, "pubsub_create"}}},
        {"pubsub:manage", {{CapabilityDomain::Orchestration, "pubsub_manage"}}},
        // Orchestration — the pipeline authoring/deploy/run + serving + cloud
        // control surface. The capability model is exact-match (no wildcards), so
        // every registry Orchestration op must appear here or NO role can grant it.
        // These went unwired while the only way to reach orchestration over
        // HTTP/MCP was the unauthenticated-local bypass (which skipped the
        // capability check); with auth always on (#799) the bootstrap owner (admin
        // = the whole vocabulary) must actually hold them. Keep in sync with the
        // registry's Orchestration capabilities (capabilities.cpp /
        // cloud_capabilities.cpp) — the admin-covers-every-op invariant is
        // guarded by a test (tests/api/test_auth_gate.cpp).
        {"pipelines:manage",
         {{CapabilityDomain::Orchestration, "create_project"},
          {CapabilityDomain::Orchestration, "delete_project"},
          {CapabilityDomain::Orchestration, "add_dataset"},
          {CapabilityDomain::Orchestration, "create_dataset_from_query"},
          {CapabilityDomain::Orchestration, "promote_dataset"},
          {CapabilityDomain::Orchestration, "create_connection"},
          {CapabilityDomain::Orchestration, "list_connections"},
          {CapabilityDomain::Orchestration, "list_source_tables"},
          {CapabilityDomain::Orchestration, "delete_connection"},
          {CapabilityDomain::Orchestration, "create_index"},
          {CapabilityDomain::Orchestration, "save_graph"},
          {CapabilityDomain::Orchestration, "capture_intent"},
          {CapabilityDomain::Orchestration, "import_n8n"},
          {CapabilityDomain::Orchestration, "compile_intent"},
          {CapabilityDomain::Orchestration, "compile_n8n"},
          {CapabilityDomain::Orchestration, "compile_python"},
          {CapabilityDomain::Orchestration, "approve_plan"},
          {CapabilityDomain::Orchestration, "gate"},
          {CapabilityDomain::Orchestration, "deploy"},
          {CapabilityDomain::Orchestration, "run"},
          {CapabilityDomain::Orchestration, "start_run"},
          {CapabilityDomain::Orchestration, "testsuite_run"},
          {CapabilityDomain::Orchestration, "create_agent"},
          {CapabilityDomain::Orchestration, "update_agent"},
          {CapabilityDomain::Orchestration, "agent_training_status"},
          {CapabilityDomain::Orchestration, "set_training_schedule"},
          {CapabilityDomain::Orchestration, "delete_agent"},
          {CapabilityDomain::Orchestration, "create_workspace_profile"},
          {CapabilityDomain::Orchestration, "update_workspace_profile"},
          {CapabilityDomain::Orchestration, "create_plan"},
          {CapabilityDomain::Orchestration, "update_plan"},
          {CapabilityDomain::Orchestration, "record_action"},
          {CapabilityDomain::Orchestration, "record_outcome"}}},
        {"serving:manage",
         {{CapabilityDomain::Orchestration, "expose_model"},
          {CapabilityDomain::Orchestration, "unexpose_model"},
          {CapabilityDomain::Orchestration, "expose_index"},
          {CapabilityDomain::Orchestration, "unexpose_index"}}},
        {"cloud:manage",
         {{CapabilityDomain::Orchestration, "cloud_inspect"},
          {CapabilityDomain::Orchestration, "detect_provisioners"},
          {CapabilityDomain::Orchestration, "add_nodes"},
          {CapabilityDomain::Orchestration, "approve_consent"},
          {CapabilityDomain::Orchestration, "deny_consent"}}},
    };
    return entries;
}

const PermissionEntry* find_entry(std::string_view name) {
    for (const PermissionEntry& entry : vocabulary()) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

}  // namespace

const std::vector<std::string>& permission_vocabulary() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        out.reserve(vocabulary().size());
        for (const PermissionEntry& entry : vocabulary()) {
            out.emplace_back(entry.name);
        }
        return out;
    }();
    return names;
}

bool is_permission(std::string_view name) { return find_entry(name) != nullptr; }

std::vector<Operation> permission_operations(std::string_view name) {
    if (const PermissionEntry* entry = find_entry(name)) return entry->operations;
    return {};
}

CapabilitySet capabilities_for_permissions(
    const std::vector<std::string>& names) {
    CapabilitySet capabilities;
    for (const std::string& name : names) {
        for (const Operation& op : permission_operations(name)) {
            // De-duplicate: two permissions may share an operation.
            const Operation right{op.domain, op.action};
            bool present = false;
            for (const Capability& existing : capabilities.grants) {
                if (existing.right() == right) {
                    present = true;
                    break;
                }
            }
            if (!present) capabilities.grant(op.domain, op.action);
        }
    }
    return capabilities;
}

std::vector<std::string> permissions_of(const CapabilitySet& capabilities,
                                        std::string_view now) {
    std::vector<std::string> names;
    for (const PermissionEntry& entry : vocabulary()) {
        bool all = !entry.operations.empty();
        for (const Operation& op : entry.operations) {
            if (!capabilities.permits(op, now)) {
                all = false;
                break;
            }
        }
        if (all) names.emplace_back(entry.name);
    }
    return names;
}

std::optional<std::vector<std::string>> builtin_role_permission_names(
    std::string_view role) {
    if (role == "admin") return permission_vocabulary();  // everything
    if (role == "writer") {
        return std::vector<std::string>{"sql:read", "sql:write", "sql:ddl",
                                        "mcp:call", "serving:read"};
    }
    if (role == "reader") {
        return std::vector<std::string>{"sql:read", "serving:read", "mcp:call"};
    }
    return std::nullopt;
}

}  // namespace zetta::governance
