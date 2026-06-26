#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/access_key.hpp"
#include "zetta/cluster.hpp"
#include "zetta/node.hpp"
#include "zetta/principal.hpp"
#include "zetta/replication.hpp"
#include "zetta/scope.hpp"
#include "zetta/uuid.hpp"

using zetta::AccessDecision;
using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::PrincipalKind;
using zetta::ReplicationManager;
using zetta::Resource;
using zetta::ResourceKind;
using zetta::Scope;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

Resource database(std::string name) {
    return Resource{ResourceKind::Database, std::move(name)};
}
Resource project(std::string name) {
    return Resource{ResourceKind::Project, std::move(name)};
}

// Tick both nodes until `done` holds (or we give up). Mirrors the project
// replication test: capture -> cluster gossip -> peer merge -> reconcile.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

}  // namespace

// ----- Scope: the deny-by-default enforcement primitive (RULE-3) -------------

TEST(Scope, DenyByDefault) {
    Scope scope;  // grants nothing
    EXPECT_FALSE(scope.permits(database("churn")));
    EXPECT_FALSE(scope.permits(project("churn")));
}

TEST(Scope, PermitsExactlyWhatIsGranted) {
    Scope scope;
    scope.grant(ResourceKind::Database, "churn")
        .grant(ResourceKind::Project, "retention");

    EXPECT_TRUE(scope.permits(database("churn")));
    EXPECT_TRUE(scope.permits(project("retention")));

    // A different name is out of scope...
    EXPECT_FALSE(scope.permits(database("fraud")));
    // ...and so is the same name under a different kind (no cross-kind grant).
    EXPECT_FALSE(scope.permits(project("churn")));
    EXPECT_FALSE(scope.permits(database("retention")));
}

TEST(Scope, UnrestrictedPermitsEverything) {
    const Scope scope = zetta::unrestricted_scope();
    EXPECT_TRUE(scope.permits(database("anything")));
    EXPECT_TRUE(scope.permits(project("anything")));
    EXPECT_TRUE(scope.permits(database("")));
}

// ----- authorize(): allow/deny + a human reason ------------------------------

TEST(Authorize, AllowsInScopeAndDeniesOutOfScope) {
    Scope scope;
    scope.grant(ResourceKind::Database, "churn");
    const zetta::AccessKey key{"key-id", "principal-id", scope, "2026-01-01",
                               TempDir{}.path()};

    const AccessDecision allowed = zetta::authorize(key, database("churn"));
    EXPECT_TRUE(allowed.allowed);
    EXPECT_TRUE(static_cast<bool>(allowed));
    EXPECT_NE(allowed.reason.find("in scope"), std::string::npos);

    const AccessDecision denied = zetta::authorize(key, database("fraud"));
    EXPECT_FALSE(denied.allowed);
    EXPECT_NE(denied.reason.find("outside"), std::string::npos);
    EXPECT_NE(denied.reason.find("fraud"), std::string::npos);
}

// ----- JSON round-trip + boundary validation ---------------------------------

TEST(Scope, RoundTripsThroughJson) {
    Scope scope;
    scope.unrestricted = false;
    scope.grant(ResourceKind::Database, "churn")
        .grant(ResourceKind::Project, "retention");

    const nlohmann::json document = scope;  // to_json ADL hook
    Scope parsed;
    ASSERT_TRUE(zetta::scope_from_json(document, parsed));
    EXPECT_EQ(parsed, scope);
}

TEST(Scope, RejectsMalformedJsonAtTheBoundary) {
    Scope out;
    // Not an object.
    EXPECT_FALSE(zetta::scope_from_json(nlohmann::json::array(), out));
    // Missing `grants`.
    EXPECT_FALSE(zetta::scope_from_json(
        nlohmann::json{{"unrestricted", false}}, out));
    // `unrestricted` wrong type.
    EXPECT_FALSE(zetta::scope_from_json(
        nlohmann::json{{"unrestricted", "no"}, {"grants", nlohmann::json::array()}},
        out));
    // An unknown resource kind must fail fast (never silently drop a grant).
    EXPECT_FALSE(zetta::scope_from_json(
        nlohmann::json::parse(
            R"({"unrestricted": false, "grants": [{"kind": "table", "name": "x"}]})"),
        out));
}

TEST(AccessKey, MetadataCarriesPrincipalAndScope) {
    Scope scope;
    scope.grant(ResourceKind::Project, "churn");
    const zetta::AccessKey key{"key-id", "principal-id", scope, "2026-01-01",
                               TempDir{}.path()};
    const nlohmann::json meta = key.metadata();
    EXPECT_EQ(meta.at("principal"), "principal-id");
    EXPECT_EQ(meta.at("created"), "2026-01-01");

    Scope parsed;
    ASSERT_TRUE(zetta::scope_from_json(meta.at("scope"), parsed));
    EXPECT_EQ(parsed, scope);
}

TEST(Principal, KindRoundTripsThroughStrings) {
    for (const auto kind : {PrincipalKind::User, PrincipalKind::Service,
                            PrincipalKind::Node}) {
        const auto parsed = zetta::principal_kind_from(zetta::to_string(kind));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, kind);
    }
    EXPECT_FALSE(zetta::principal_kind_from("robot").has_value());
}

// ----- Node CRUD + the key-scoped access decision (the acceptance test) ------

TEST(AccessControl, KeyScopedPrincipalIsAllowedInScopeAndDeniedOutOfScope) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // A principal with a key scoped to exactly the `churn` database + project.
    auto analyst = node.create_principal("analyst", PrincipalKind::User);
    ASSERT_TRUE(analyst);
    Scope scope;
    scope.grant(ResourceKind::Database, "churn")
        .grant(ResourceKind::Project, "churn");
    auto key = analyst->create_key(scope);
    ASSERT_TRUE(key);

    // A request that logs in with the key inherits its scope: in-scope allowed.
    EXPECT_TRUE(node.authorize(key->id(), database("churn")).allowed);
    EXPECT_TRUE(node.authorize(key->id(), project("churn")).allowed);

    // Out-of-scope denied — a different resource the key was never granted.
    EXPECT_FALSE(node.authorize(key->id(), database("fraud")).allowed);
    EXPECT_FALSE(node.authorize(key->id(), project("retention")).allowed);

    // Deny-by-default for an unknown / empty key id (no credential => no access).
    EXPECT_FALSE(node.authorize("no-such-key", database("churn")).allowed);
    EXPECT_FALSE(node.authorize("", database("churn")).allowed);

    node.stop();
}

TEST(AccessControl, UnrestrictedKeyReachesEverything) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto admin = node.create_principal("admin", PrincipalKind::Service);
    ASSERT_TRUE(admin);
    auto key = admin->create_key(zetta::unrestricted_scope());
    ASSERT_TRUE(key);
    EXPECT_TRUE(node.authorize(key->id(), database("anything")).allowed);
    EXPECT_TRUE(node.authorize(key->id(), project("else")).allowed);
    node.stop();
}

TEST(Principal, CreateFindDropRename) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto alice = node.create_principal("alice");
    ASSERT_TRUE(alice);
    EXPECT_EQ(alice->kind(), PrincipalKind::User);  // default kind

    EXPECT_FALSE(node.create_principal("alice"));  // duplicate
    EXPECT_FALSE(node.create_principal(""));        // empty

    auto svc = node.create_principal("ingest", PrincipalKind::Service);
    ASSERT_TRUE(svc);
    EXPECT_EQ(svc->kind(), PrincipalKind::Service);

    EXPECT_EQ(node.principal_count(), 2u);
    EXPECT_TRUE(node.find_principal("alice"));
    EXPECT_FALSE(node.find_principal("nobody"));

    EXPECT_TRUE(node.rename_principal("alice", "alice2"));
    EXPECT_TRUE(node.find_principal("alice2"));
    EXPECT_FALSE(node.find_principal("alice"));
    EXPECT_FALSE(node.rename_principal("alice2", "ingest"));  // target taken

    // Drop removes the principal and its keys.
    auto with_key = node.find_principal("alice2");
    ASSERT_TRUE(with_key);
    with_key->create_key(zetta::unrestricted_scope());
    EXPECT_TRUE(node.drop_principal("alice2"));
    EXPECT_EQ(node.principal_count(), 1u);
    EXPECT_FALSE(node.drop_principal("alice2"));  // already gone

    node.stop();
}

// ----- persistence + replication ---------------------------------------------

TEST(AccessControl, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string principal_id;
    std::string key_id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto principal = node.create_principal("analyst");
        ASSERT_TRUE(principal);
        Scope scope;
        scope.grant(ResourceKind::Database, "churn");
        auto key = principal->create_key(scope);
        ASSERT_TRUE(key);
        principal_id = principal->id();
        key_id = key->id();
        node.stop();
    }

    // A fresh standalone node reads the principal + key purely from disk and the
    // scope still enforces.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.principal_count(), 1u);
    auto principal = reloaded.find_principal("analyst");
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal->id(), principal_id);
    auto key = reloaded.find_access_key(key_id);
    ASSERT_TRUE(key);
    EXPECT_TRUE(reloaded.authorize(key_id, database("churn")).allowed);
    EXPECT_FALSE(reloaded.authorize(key_id, database("fraud")).allowed);
    reloaded.stop();
}

TEST(AccessControl, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::new_uuid();
    std::string key_id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto principal = node.create_principal("analyst");
        ASSERT_TRUE(principal);
        Scope scope;
        scope.grant(ResourceKind::Project, "churn");
        auto key = principal->create_key(scope);
        ASSERT_TRUE(key);
        key_id = key->id();
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document; the
    // principal + key are rebuilt from it and the scope still enforces.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int principal_keys = 0;
    int access_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("principal:", 0) == 0) ++principal_keys;
        if (key.rfind("key:", 0) == 0) ++access_keys;
    }
    EXPECT_EQ(principal_keys, 1);
    EXPECT_EQ(access_keys, 1);

    EXPECT_TRUE(node.authorize(key_id, project("churn")).allowed);
    EXPECT_FALSE(node.authorize(key_id, project("fraud")).allowed);
    node.stop();
}

TEST(AccessControl, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43320;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43321;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43320"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create a principal + scoped key on node A only.
    auto principal = a.create_principal("analyst");
    ASSERT_TRUE(principal);
    Scope scope;
    scope.grant(ResourceKind::Database, "churn");
    auto key = principal->create_key(scope);
    ASSERT_TRUE(key);
    const std::string key_id = key->id();

    // Node B converges to the same principal + key, and the key's scope enforces
    // identically on B (the access decision replicated, not just the bytes).
    const bool converged = converge(a, b, [&] {
        auto replicated = b.find_access_key(key_id);
        return replicated && repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    EXPECT_TRUE(b.find_principal("analyst"));
    EXPECT_TRUE(b.authorize(key_id, database("churn")).allowed);
    EXPECT_FALSE(b.authorize(key_id, database("fraud")).allowed);

    // Dropping the principal on A also removes the key on B.
    ASSERT_TRUE(a.drop_principal("analyst"));
    const bool dropped = converge(a, b, [&] {
        return !b.find_principal("analyst") && !b.find_access_key(key_id);
    });
    EXPECT_TRUE(dropped);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
