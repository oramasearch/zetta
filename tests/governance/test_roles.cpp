// RBAC roles (FEAT-9 / #746, epic #743): the permission vocabulary, the
// complete built-in admin/writer/reader sets reconciled idempotently on
// bootstrap, custom roles created + edited via SQL and gated on roles:manage,
// user↔role assignment (effective permissions = the union across roles), and the
// system.roles / system.user_roles read views. Layered on #744's store +
// Node::authorize_user (#745).
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/permission.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// Does a user's effective (role-union) reach cover this domain:action?
bool can(const Node& node, const std::string& user_id, CapabilityDomain domain,
         const std::string& action) {
    return node.authorize_user(user_id, Operation{domain, action}).allowed;
}

}  // namespace

// ===== the permission vocabulary =============================================

TEST(PermissionVocabulary, MapsAtomsOntoCapabilityDomainAction) {
    using zetta::governance::is_permission;
    using zetta::governance::permission_operations;

    // Every canonical atom is known and maps onto the documented operation(s).
    EXPECT_TRUE(is_permission("sql:read"));
    EXPECT_TRUE(is_permission("roles:manage"));
    EXPECT_FALSE(is_permission("sql:teleport"));       // not in the vocabulary
    EXPECT_FALSE(is_permission("bogus"));

    EXPECT_EQ(permission_operations("sql:read"),
              (std::vector<Operation>{{CapabilityDomain::Data, "read"}}));
    EXPECT_EQ(permission_operations("sql:write"),
              (std::vector<Operation>{{CapabilityDomain::Data, "write"}}));
    EXPECT_EQ(permission_operations("sql:ddl"),
              (std::vector<Operation>{{CapabilityDomain::Data, "ddl"}}));
    EXPECT_EQ(permission_operations("mcp:call"),
              (std::vector<Operation>{{CapabilityDomain::Api, "query"}}));
    // blob:read — the object-scopable large-object read right (#747).
    EXPECT_EQ(permission_operations("blob:read"),
              (std::vector<Operation>{{CapabilityDomain::Data, "blob_read"}}));
    // serving:read expands to BOTH governed serving routes.
    EXPECT_EQ(permission_operations("serving:read"),
              (std::vector<Operation>{{CapabilityDomain::Data, "predict"},
                                      {CapabilityDomain::Data, "search"}}));
    EXPECT_EQ(permission_operations("users:manage"),
              (std::vector<Operation>{{CapabilityDomain::Api, "manage_users"}}));
    EXPECT_EQ(permission_operations("roles:manage"),
              (std::vector<Operation>{{CapabilityDomain::Api, "manage_roles"}}));
    EXPECT_EQ(permission_operations("tokens:manage"),
              (std::vector<Operation>{{CapabilityDomain::Api, "manage_tokens"}}));
    EXPECT_EQ(permission_operations("cluster:admin"),
              (std::vector<Operation>{{CapabilityDomain::Api, "cluster_admin"}}));

    // Orchestration atoms (#799): the pipeline authoring/deploy/run + serving +
    // cloud control surface, so the admin/owner can drive it under always-on auth.
    EXPECT_TRUE(is_permission("pipelines:manage"));
    EXPECT_TRUE(is_permission("serving:manage"));
    EXPECT_TRUE(is_permission("cloud:manage"));
    EXPECT_EQ(permission_operations("serving:manage"),
              (std::vector<Operation>{
                  {CapabilityDomain::Orchestration, "expose_model"},
                  {CapabilityDomain::Orchestration, "unexpose_model"},
                  {CapabilityDomain::Orchestration, "expose_index"},
                  {CapabilityDomain::Orchestration, "unexpose_index"}}));
    EXPECT_EQ(permission_operations("cloud:manage"),
              (std::vector<Operation>{
                  {CapabilityDomain::Orchestration, "cloud_inspect"},
                  {CapabilityDomain::Orchestration, "detect_provisioners"},
                  {CapabilityDomain::Orchestration, "add_nodes"},
                  {CapabilityDomain::Orchestration, "approve_consent"},
                  {CapabilityDomain::Orchestration, "deny_consent"}}));
    // pipelines:manage expands to the full authoring/deploy/run flow
    // (create_index joined it, #825; promote_dataset, #833; the agent
    // authoring trio create/update/delete_agent, #849; approve_plan, #885;
    // create/update_workspace_profile, #886).
    EXPECT_EQ(permission_operations("pipelines:manage").size(), 25u);

    EXPECT_EQ(permission_operations("nope"), std::vector<Operation>{});

    // The canonical list carries the #746 atoms, blob:read (#747), and the three
    // Orchestration atoms (#799).
    EXPECT_EQ(zetta::governance::permission_vocabulary().size(), 13u);
}

TEST(PermissionVocabulary, ExpandAndReverseRoundTrip) {
    using zetta::governance::capabilities_for_permissions;
    using zetta::governance::permissions_of;
    const std::string now = "2026-07-10T00:00:00.000Z";

    // A subset expands to exactly its operations (de-duplicated union) and reads
    // back as exactly the subset it satisfies.
    const std::vector<std::string> subset = {"sql:read", "serving:read"};
    const auto caps = capabilities_for_permissions(subset);
    EXPECT_TRUE(caps.permits({CapabilityDomain::Data, "read"}, now));
    EXPECT_TRUE(caps.permits({CapabilityDomain::Data, "predict"}, now));
    EXPECT_TRUE(caps.permits({CapabilityDomain::Data, "search"}, now));
    EXPECT_FALSE(caps.permits({CapabilityDomain::Data, "write"}, now));

    EXPECT_EQ(permissions_of(caps, now), subset);

    // serving:read is "present" only when BOTH its operations are granted —
    // predict alone does not satisfy it.
    const auto predict_only =
        capabilities_for_permissions(std::vector<std::string>{"sql:read"});
    zetta::governance::CapabilitySet partial = predict_only;
    partial.grant(CapabilityDomain::Data, "predict");
    const auto present = permissions_of(partial, now);
    EXPECT_NE(std::find(present.begin(), present.end(), "sql:read"), present.end());
    EXPECT_EQ(std::find(present.begin(), present.end(), "serving:read"),
              present.end());
}

// ===== built-in roles: complete + seeded + reconciled ========================

TEST(BuiltinRoles, PermissionSetsMatchTheIssue) {
    using zetta::governance::builtin_role_permissions;
    using zetta::governance::permissions_of;
    const std::string now = "2026-07-10T00:00:00.000Z";

    const auto admin = builtin_role_permissions("admin");
    ASSERT_TRUE(admin);
    // admin = every permission incl. the *:manage + cluster:admin atoms.
    EXPECT_EQ(permissions_of(admin->capabilities, now),
              zetta::governance::permission_vocabulary());
    EXPECT_TRUE(admin->scope.unrestricted);

    const auto writer = builtin_role_permissions("writer");
    ASSERT_TRUE(writer);
    EXPECT_EQ(permissions_of(writer->capabilities, now),
              (std::vector<std::string>{"sql:read", "sql:write", "sql:ddl",
                                        "mcp:call", "serving:read"}));

    const auto reader = builtin_role_permissions("reader");
    ASSERT_TRUE(reader);
    EXPECT_EQ(permissions_of(reader->capabilities, now),
              (std::vector<std::string>{"sql:read", "mcp:call", "serving:read"}));

    EXPECT_FALSE(builtin_role_permissions("nope"));
}

TEST(BuiltinRoles, ExistOnBootstrapAndReconcileIdempotently) {
    TempDir dir;
    Node node{config_for(dir)};
    ASSERT_TRUE(node.start());

    // Bootstrap seeds the built-ins (via the owner) — all three exist.
    ASSERT_TRUE(node.bootstrap_owner("secret").performed);
    EXPECT_TRUE(node.find_role("admin"));
    EXPECT_TRUE(node.find_role("writer"));
    EXPECT_TRUE(node.find_role("reader"));
    const std::size_t roles_after_bootstrap = node.role_count();

    // ensure_builtin_roles is idempotent — a second call creates nothing new and
    // does not duplicate.
    EXPECT_EQ(node.ensure_builtin_roles(), 0u);
    EXPECT_EQ(node.role_count(), roles_after_bootstrap);

    // Drift repair: mutate admin away from its canonical set, then reconcile.
    auto admin = node.find_role("admin");
    ASSERT_TRUE(admin);
    ASSERT_TRUE(node.revoke_permissions("admin", {"roles:manage"}));
    EXPECT_FALSE(can(node, "", CapabilityDomain::Api, "manage_roles"));  // gone
    node.ensure_builtin_roles();  // reconciles admin back to canonical
    admin = node.find_role("admin");
    const std::string now = "2026-07-10T00:00:00.000Z";
    EXPECT_TRUE(admin->capabilities().permits(
        {CapabilityDomain::Api, "manage_roles"}, now));
    node.stop();
}

// ===== custom roles: a permission subset grants exactly those ================

TEST(CustomRoles, GrantExactlyTheSubsetAndAssignmentChangesReach) {
    TempDir dir;
    Node node{config_for(dir)};
    ASSERT_TRUE(node.start());

    // A custom role starts empty (deny-by-default), then gains exactly the
    // granted permissions — nothing more.
    auto role = node.create_role("analyst");
    ASSERT_TRUE(role);
    ASSERT_TRUE(node.grant_permissions("analyst", {"sql:read", "serving:read"}));

    auto user = node.create_user("dana");
    ASSERT_TRUE(user);

    // Before assignment: no reach at all.
    EXPECT_FALSE(can(node, user->id(), CapabilityDomain::Data, "read"));

    // Assigning the role grants exactly its permissions.
    ASSERT_TRUE(node.assign_role(user->id(), role->id()));
    EXPECT_TRUE(can(node, user->id(), CapabilityDomain::Data, "read"));      // sql:read
    EXPECT_TRUE(can(node, user->id(), CapabilityDomain::Data, "predict"));   // serving:read
    EXPECT_TRUE(can(node, user->id(), CapabilityDomain::Data, "search"));    // serving:read
    EXPECT_FALSE(can(node, user->id(), CapabilityDomain::Data, "write"));    // not granted
    EXPECT_FALSE(can(node, user->id(), CapabilityDomain::Api, "manage_roles"));

    // Revoking a permission narrows the role (and thus the user's reach).
    ASSERT_TRUE(node.revoke_permissions("analyst", {"serving:read"}));
    EXPECT_TRUE(can(node, user->id(), CapabilityDomain::Data, "read"));
    EXPECT_FALSE(can(node, user->id(), CapabilityDomain::Data, "predict"));

    // Unassigning the role removes the reach entirely.
    ASSERT_TRUE(node.unassign_role(user->id(), role->id()));
    EXPECT_FALSE(can(node, user->id(), CapabilityDomain::Data, "read"));

    // Custom-role guards: empty name / clash rejected.
    EXPECT_FALSE(node.create_role(""));
    EXPECT_FALSE(node.create_role("analyst"));  // clash
    node.stop();
}

TEST(CustomRoles, EffectivePermissionsAreTheUnionAcrossRoles) {
    TempDir dir;
    Node node{config_for(dir)};
    ASSERT_TRUE(node.start());

    auto reads = node.create_role("reads");
    auto writes = node.create_role("writes");
    ASSERT_TRUE(reads && writes);
    ASSERT_TRUE(node.grant_permissions("reads", {"sql:read"}));
    ASSERT_TRUE(node.grant_permissions("writes", {"sql:write"}));

    auto user = node.create_user("multi");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), reads->id()));
    ASSERT_TRUE(node.assign_role(user->id(), writes->id()));

    // The union of the two roles' rights — read AND write, neither alone.
    EXPECT_TRUE(can(node, user->id(), CapabilityDomain::Data, "read"));
    EXPECT_TRUE(can(node, user->id(), CapabilityDomain::Data, "write"));
    EXPECT_FALSE(can(node, user->id(), CapabilityDomain::Data, "ddl"));

    // Dropping one role removes only its contribution.
    ASSERT_TRUE(node.drop_role("writes"));
    EXPECT_TRUE(can(node, user->id(), CapabilityDomain::Data, "read"));
    EXPECT_FALSE(can(node, user->id(), CapabilityDomain::Data, "write"));
    // Dropping a role also removes its assignments (no dangling user_role).
    EXPECT_EQ(node.user_role_count(), 1u);
    node.stop();
}

// ===== the SQL grammar, end to end ==========================================

TEST(RoleSql, CreateGrantAssignRevokeDropEndToEnd) {
    TempDir dir;
    Node node{config_for(dir)};
    ASSERT_TRUE(node.start());
    node.create_user("erin");
    QueryService svc{node};  // local session: trusted, roles:manage not required

    ASSERT_TRUE(svc.run("CREATE ROLE reviewer;", "").ok());
    ASSERT_TRUE(node.find_role("reviewer"));

    // GRANT a permission list onto the role.
    ASSERT_TRUE(svc.run("GRANT sql:read, sql:write TO ROLE reviewer;", "").ok());
    auto role = node.find_role("reviewer");
    const std::string now = "2026-07-10T00:00:00.000Z";
    EXPECT_TRUE(role->capabilities().permits({CapabilityDomain::Data, "read"}, now));
    EXPECT_TRUE(role->capabilities().permits({CapabilityDomain::Data, "write"}, now));

    // An unknown permission fails the whole statement.
    auto bad = svc.run("GRANT sql:teleport TO ROLE reviewer;", "");
    EXPECT_FALSE(bad.ok());
    EXPECT_NE(bad.error.find("unknown permission"), std::string::npos);

    // REVOKE narrows it.
    ASSERT_TRUE(svc.run("REVOKE sql:write FROM ROLE reviewer;", "").ok());
    role = node.find_role("reviewer");
    EXPECT_FALSE(role->capabilities().permits({CapabilityDomain::Data, "write"}, now));

    // Assign the role to a user, then unassign.
    auto erin = node.find_user("erin");
    ASSERT_TRUE(erin);
    ASSERT_TRUE(svc.run("GRANT ROLE reviewer TO erin;", "").ok());
    EXPECT_TRUE(can(node, erin->id(), CapabilityDomain::Data, "read"));
    ASSERT_TRUE(svc.run("REVOKE ROLE reviewer FROM erin;", "").ok());
    EXPECT_FALSE(can(node, erin->id(), CapabilityDomain::Data, "read"));

    // DROP the role.
    ASSERT_TRUE(svc.run("DROP ROLE reviewer;", "").ok());
    EXPECT_FALSE(node.find_role("reviewer"));

    // Clear errors on unknown targets.
    EXPECT_FALSE(svc.run("GRANT sql:read TO ROLE nope;", "").ok());
    EXPECT_FALSE(svc.run("DROP ROLE nope;", "").ok());
    EXPECT_FALSE(svc.run("GRANT ROLE nope TO erin;", "").ok());
    node.stop();
}

TEST(RoleSql, ManagingRolesRequiresRolesManage) {
    TempDir dir;
    Node node{config_for(dir)};
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();

    // An admin user (has roles:manage) and a reader user (does not).
    auto admin_user = node.create_user("boss");
    auto reader_user = node.create_user("peon");
    ASSERT_TRUE(admin_user && reader_user);
    node.assign_role(admin_user->id(), node.find_role("admin")->id());
    node.assign_role(reader_user->id(), node.find_role("reader")->id());

    // Acting AS the reader user (the #745 gate resolved a token -> user): denied.
    QueryService reader_svc{node, nullptr, reader_user->id(), /*acting_is_user=*/true};
    auto denied = reader_svc.run("CREATE ROLE sneaky;", "");
    EXPECT_FALSE(denied.ok());
    EXPECT_NE(denied.error.find("manage_roles"), std::string::npos);
    EXPECT_FALSE(node.find_role("sneaky"));  // nothing was created

    // The reader can still read (SELECT is not gated on roles:manage).
    EXPECT_TRUE(reader_svc.run("SELECT * FROM system.roles;", "").ok());

    // Acting AS the admin user: allowed.
    QueryService admin_svc{node, nullptr, admin_user->id(), /*acting_is_user=*/true};
    EXPECT_TRUE(admin_svc.run("CREATE ROLE approved;", "").ok());
    EXPECT_TRUE(node.find_role("approved"));

    // The trusted local session (no acting user) is always allowed.
    QueryService local_svc{node};
    EXPECT_TRUE(local_svc.run("CREATE ROLE localrole;", "").ok());
    node.stop();
}

// ===== read views: system.roles (permission set) + system.user_roles =========

TEST(RoleViews, RolesShowPermissionSetAndUserRolesJoin) {
    TempDir dir;
    Node node{config_for(dir)};
    ASSERT_TRUE(node.start());
    auto role = node.create_role("viewer");
    ASSERT_TRUE(role);
    ASSERT_TRUE(node.grant_permissions("viewer", {"sql:read", "mcp:call"}));
    auto user = node.create_user("val");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), role->id()));

    QueryService svc{node};

    // system.roles carries the role's permission set in the RBAC vocabulary.
    auto roles = svc.run("SELECT * FROM system.roles;", "");
    ASSERT_TRUE(roles.ok()) << roles.error;
    ASSERT_EQ(roles.rows.size(), 1u);
    EXPECT_EQ(roles.rows[0]["name"], "viewer");
    EXPECT_EQ(roles.rows[0]["permissions"], "sql:read, mcp:call");

    // system.user_roles is the (user, role) join.
    auto user_roles = svc.run("SELECT * FROM system.user_roles;", "");
    ASSERT_TRUE(user_roles.ok()) << user_roles.error;
    ASSERT_EQ(user_roles.rows.size(), 1u);
    EXPECT_EQ(user_roles.rows[0]["user"], "val");
    EXPECT_EQ(user_roles.rows[0]["role"], "viewer");
    node.stop();
}
