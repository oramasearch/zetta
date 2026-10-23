// The authorization regression harness (#780): one group of tests per finding of
// the auth review, #759 through #769, run in-process against the headless core so
// the full ctest proves them on every build. The black-box half is §3.13 of
// tests/e2e/zetta-e2e.sh, run against a deployed node.
//
// Two kinds of test live here:
//   - a finding that is fixed on `dev` gets a plain assertion: if it comes back,
//     the test fails;
//   - a finding that is still open gets a KNOWN_DEFECT probe. The probe runs the
//     reproduction; while the defect reproduces the test SKIPs with the issue
//     number, and the moment it no longer reproduces the test FAILS, telling
//     whoever fixed it to replace the probe with the plain assertion written next
//     to it. So a fix cannot land silently, and once flipped, a regression fails.
//
// The finding each test covers is in its name and its comment. The coverage map,
// with what is fixed and what is open, is docs/testing/authz-regressions.md.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/server.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/permission.hpp"
#include "zetta/governance/policy.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/governance/user_role.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::AuditAction;
using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::governance::PolicyValue;
using zetta::governance::Resource;
using zetta::governance::TokenType;
using zetta::query::QueryService;
using zetta::test::TempDir;

// A still-open finding. `still_present` is the reproduction's verdict. While it
// holds, SKIP and name the issue; once it stops holding, FAIL so the probe is
// replaced by the plain assertion beside it (strict expected-failure).
#define KNOWN_DEFECT(issue, still_present, what)                                  \
    do {                                                                          \
        if (still_present) {                                                      \
            GTEST_SKIP() << "open defect zetta#" #issue ": " << (what);           \
        }                                                                         \
        FAIL() << "zetta#" #issue " no longer reproduces (" << (what)             \
               << "). It looks fixed: replace this KNOWN_DEFECT probe with the "  \
                  "plain assertion next to it, and update "                       \
                  "docs/testing/authz-regressions.md.";                           \
    } while (false)

namespace {

constexpr const char* kOwnerSecret = "authz-regression-owner-secret";

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    cfg.preflight = false;
    return cfg;
}

std::string user_with_roles(Node& node, const std::string& name,
                            const std::vector<std::string>& role_names) {
    auto user = node.create_user(name);
    EXPECT_TRUE(user) << name;
    for (const std::string& role_name : role_names) {
        auto role = node.find_role(role_name);
        EXPECT_TRUE(role) << role_name;
        if (user && role) node.assign_role(user->id(), role->id());
    }
    return user ? user->id() : std::string{};
}

// Refused for want of a right: the statement fails with a permission error
// (the executor's "permission denied", or the role check's "no role grants"),
// never a parse error that would pass for a refusal.
bool refused(const QueryService::Outcome& result) {
    return !result.ok() &&
           (result.error.find("permission denied") != std::string::npos ||
            result.error.find("no role grants") != std::string::npos);
}

// One node, its owner seeded the way --admin-token seeds it, the built-in roles,
// and a small table: app.people (id, owner, salary).
struct AuthzNode : ::testing::Test {
    TempDir dir;
    Node node{config_for(dir)};
    QueryService local{node};  // the trusted local session builds the fixture

    void SetUp() override {
        ASSERT_TRUE(node.start());
        const auto boot = node.bootstrap_owner(kOwnerSecret);
        ASSERT_TRUE(boot.performed) << boot.reason;
        node.ensure_builtin_roles();
        for (const char* sql :
             {"CREATE DATABASE app;", "CREATE TABLE app.people (id int);",
              "INSERT INTO app.people (id, owner, salary) VALUES "
              "(1, 'ada', 91000), (2, 'bo', 87000), (3, 'ada', 123000);"}) {
            const auto r = local.run(sql, "");
            ASSERT_TRUE(r.ok()) << r.error << "  (" << sql << ")";
        }
    }
    void TearDown() override { node.stop(); }

    QueryService as(const std::string& user_id) {
        return QueryService{node, nullptr, user_id, /*acting_is_user=*/true};
    }
    void run_local(const std::string& sql) {
        const auto r = local.run(sql, "");
        ASSERT_TRUE(r.ok()) << r.error << "  (" << sql << ")";
    }
    std::size_t tokens_for(const std::string& user_name) {
        auto user = node.find_user(user_name);
        if (!user) return 0;
        std::size_t count = 0;
        for (const auto& token : node.tokens()) {
            if (token->user_id() == user->id()) ++count;
        }
        return count;
    }
};

}  // namespace

// ===== #759: privileged SQL skipped its permission check ======================
// Fixed on dev by #1229: CREATE TOKEN / CREATE USER need tokens:manage /
// users:manage, and the schema statements need sql:ddl.

TEST_F(AuthzNode, Finding759_ReaderCannotMintTheOwnerTokenOrRunDdl) {
    const std::string reader = user_with_roles(node, "rita", {"reader"});
    QueryService as_reader = as(reader);
    const std::size_t owner_tokens = tokens_for("owner");

    for (const char* sql :
         {"CREATE TOKEN FOR owner TYPE api;", "CREATE TOKEN FOR owner;",
          "CREATE USER mallory;", "CREATE DATABASE fresh;", "DROP DATABASE app;",
          "CREATE TABLE app.extra (id int);", "DROP TABLE app.people;"}) {
        const auto r = as_reader.run(sql, "");
        EXPECT_TRUE(refused(r)) << sql << ": " << r.error;
    }
    EXPECT_EQ(tokens_for("owner"), owner_tokens);  // no owner token was minted
    EXPECT_FALSE(node.find_user("mallory"));
    EXPECT_FALSE(node.find_database("fresh"));
    EXPECT_TRUE(node.find_database("app"));
    EXPECT_EQ(local.run("SELECT * FROM app.people;", "").rows.size(), 3u);
    EXPECT_TRUE(as_reader.run("SELECT * FROM app.people;", "").ok());
}

// ===== #760: column masking bypassed through an aggregate =====================
// Fixed on dev: the read guard checks an aggregate's column, not only the
// grouping key (src/core/query/executor.cpp, the column-grant guard).

TEST_F(AuthzNode, Finding760_AggregateOverAMaskedColumnIsRefused) {
    run_local("CREATE ROLE idonly;");
    run_local("GRANT sql:read ON app.people(id, owner) TO ROLE idonly;");
    const std::string clerk = user_with_roles(node, "clerk", {"idonly"});
    QueryService as_clerk = as(clerk);

    // The plain read masks salary ...
    const auto plain = as_clerk.run("SELECT * FROM app.people;", "");
    ASSERT_TRUE(plain.ok()) << plain.error;
    for (const auto& row : plain.rows) EXPECT_TRUE(row["salary"].is_null());

    // ... and no aggregate recovers it.
    for (const char* sql :
         {"SELECT id, MAX(salary) FROM app.people GROUP BY id;",
          "SELECT id, MIN(salary) FROM app.people GROUP BY id;",
          "SELECT owner, SUM(salary) FROM app.people GROUP BY owner;",
          "SELECT owner, AVG(salary) FROM app.people GROUP BY owner;"}) {
        const auto r = as_clerk.run(sql, "");
        EXPECT_TRUE(refused(r)) << sql << " returned " << nlohmann::json(r.rows).dump();
    }
}

// ===== #761: roles:manage is unbounded ========================================

TEST_F(AuthzNode, Finding761_RoleManagerCannotGrantWhatItDoesNotHold) {
    run_local("CREATE ROLE delegates;");
    run_local("GRANT roles:manage, sql:read TO ROLE delegates;");
    const std::string dee = user_with_roles(node, "dee", {"delegates"});
    QueryService as_dee = as(dee);

    const auto self_admin = as_dee.run("GRANT ROLE admin TO dee;", "");
    const auto widen = as_dee.run("GRANT tokens:manage TO ROLE delegates;", "");
    const bool escalated =
        node.authorize_user(dee, Operation{CapabilityDomain::Data, "write"}).allowed;
    // The plain assertion, once fixed:
    //   EXPECT_FALSE(self_admin.ok()); EXPECT_FALSE(widen.ok()); EXPECT_FALSE(escalated);
    KNOWN_DEFECT(761, self_admin.ok() || widen.ok() || escalated,
                 "a holder of only roles:manage granted itself admin / tokens:manage");
}

TEST_F(AuthzNode, Finding761_BuiltinRolesCannotBeEditedOrDropped) {
    const std::string boss = user_with_roles(node, "boss", {"admin"});
    QueryService as_boss = as(boss);
    const auto revoke = as_boss.run("REVOKE sql:read FROM ROLE reader;", "");
    const auto drop = as_boss.run("DROP ROLE writer;", "");
    node.tick();  // a fix may reconcile the built-ins continuously instead

    const std::string now = "2026-10-20T00:00:00.000Z";
    auto reader = node.find_role("reader");
    const bool reader_intact =
        reader && reader->capabilities().permits({CapabilityDomain::Data, "read"}, now);
    const bool writer_intact = static_cast<bool>(node.find_role("writer"));
    // The plain assertion, once fixed:
    //   EXPECT_TRUE(reader_intact); EXPECT_TRUE(writer_intact);
    (void)revoke;
    (void)drop;
    KNOWN_DEFECT(761, !reader_intact || !writer_intact,
                 "a built-in role was edited (REVOKE ... FROM ROLE reader) or "
                 "dropped (DROP ROLE writer) and stayed changed");
}

// ===== #762: authorization off by default; object controls skipped for =======
// ===== non-user identities ===================================================
// Part 1 is fixed (#799, auth always on): see Finding762 over HTTP below. Part 2:
// a token always resolves to a USER, so row security and masking bind every
// remote caller. The in-process local session and a legacy AccessKey still
// bypass object controls (Node::bypasses_row_security), but neither is reachable
// over HTTP or /mcp — asserted below by presenting an AccessKey id as a bearer.

TEST_F(AuthzNode, Finding762_RowSecurityBindsATokenResolvedUser) {
    run_local(
        "CREATE POLICY own_people ON app.people FOR SELECT "
        "USING (owner = current_user());");
    const std::string ada = user_with_roles(node, "ada", {"writer"});
    const auto r = as(ada).run("SELECT * FROM app.people;", "");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 2u);
    for (const auto& row : r.rows) EXPECT_EQ(row["owner"], "ada");
}

// ===== #765: a row policy with no condition admitted every row ================

TEST_F(AuthzNode, Finding765_APolicyWithNoConditionDeniesInsteadOfAllowingAll) {
    // Not reachable through CREATE POLICY (it requires USING); it arrives as a
    // replicated record or an edited policy.json, so it enters at the value
    // boundary the load and reconcile paths share.
    const nlohmann::json record = {{"name", "no_condition"},
                                   {"database", "app"},
                                   {"table", "people"},
                                   {"command", "select"}};
    bool fixed = false;
    std::size_t visible = 0;
    if (const auto value = zetta::governance::policy_value_from_json(record)) {
        if (node.create_policy(*value)) {
            const std::string bo = user_with_roles(node, "bo", {"writer"});
            const auto r = as(bo).run("SELECT * FROM app.people;", "");
            visible = r.ok() ? r.rows.size() : 0;
            fixed = visible == 0;
        } else {
            fixed = true;  // refused when stored
        }
    } else {
        fixed = true;  // refused at the boundary
    }
    // The plain assertion, once fixed: EXPECT_TRUE(fixed) (the record is refused,
    // or the policy admits no row).
    KNOWN_DEFECT(765, !fixed,
                 "a SELECT policy with no USING admitted " + std::to_string(visible) +
                     " of 3 rows to a non-admin");
}

// ===== #766: the policy / user / role / token catalog is readable by anyone ===

TEST_F(AuthzNode, Finding766_AReaderCannotReadTheGovernanceCatalog) {
    run_local(
        "CREATE POLICY eu_only ON app.people FOR SELECT "
        "USING (owner = 'restricted-eu');");
    const std::string reader = user_with_roles(node, "rita", {"reader"});
    QueryService as_reader = as(reader);
    std::vector<std::string> leaked;
    for (const char* view : {"system.policies", "system.users", "system.roles",
                             "system.user_roles", "system.tokens"}) {
        const auto r = as_reader.run(std::string("SELECT * FROM ") + view + ";", "");
        if (r.ok() && !r.rows.empty()) leaked.push_back(view);
    }
    std::string list;
    for (const auto& view : leaked) list += (list.empty() ? "" : ", ") + view;
    // The plain assertion, once fixed: EXPECT_TRUE(leaked.empty()) (refused, or
    // narrowed to the caller's own rows — then assert that instead).
    KNOWN_DEFECT(766, !leaked.empty(), "a reader read " + list);
}

// ===== #767: blob grants matched by text prefix, not path =====================
// Fixed by PR #1312: a blob grant covers the id and its `/`-segment children.

TEST_F(AuthzNode, Finding767_ABlobGrantStopsAtAPathBoundary) {
    node.create_role("reports");
    ASSERT_TRUE(node.grant_scoped_permissions("reports", {"blob:read"},
                                              Resource::blob("report")));
    const std::string bella = user_with_roles(node, "bella", {"reports"});
    EXPECT_TRUE(node.authorize_blob(bella, true, "report"));
    EXPECT_TRUE(node.authorize_blob(bella, true, "report/q1.pdf"));
    EXPECT_FALSE(node.authorize_blob(bella, true, "report-secret-2026"))
        << "a blob:read grant on \"report\" admitted \"report-secret-2026\"";
}

// ===== #768: orchestration could not be granted once auth was enforced ========
// Fixed by 7b1ac08: the orchestration atoms exist and admin is the whole
// vocabulary.

TEST_F(AuthzNode, Finding768_EveryRegisteredOperationIsGrantable) {
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    const std::string boss = user_with_roles(node, "boss", {"admin"});

    std::vector<Operation> grantable;
    for (const std::string& atom : zetta::governance::permission_vocabulary()) {
        for (const Operation& op : zetta::governance::permission_operations(atom)) {
            grantable.push_back(op);
        }
    }
    for (const auto& capability : registry.list()) {
        const Operation& op = capability->required_operation;
        EXPECT_TRUE(node.authorize_user(boss, op).allowed)
            << "admin cannot run " << capability->name;
        EXPECT_TRUE(std::ranges::contains(grantable, op))
            << capability->name << " needs an operation no permission grants";
    }
}

// ===== #769: hardening (owner token KDF, atomic seed, shared secret, audit) ===

TEST_F(AuthzNode, Finding769_TheOwnerTokenIsStoredThroughAKdf) {
    auto owner = node.find_user("owner");
    ASSERT_TRUE(owner);
    std::string hash;
    for (const auto& token : node.tokens()) {
        if (token->user_id() == owner->id()) hash = token->value().hash;
    }
    ASSERT_FALSE(hash.empty());
    // A KDF hash names itself (`pbkdf2-sha256$<iterations>$<hex>`, PR #1311); a
    // bare 64-hex value is a single salted SHA-256.
    const bool single_sha256 = hash.find('$') == std::string::npos;
    // The plain assertion, once fixed: EXPECT_FALSE(single_sha256).
    KNOWN_DEFECT(769, single_sha256,
                 "the --admin-token owner secret is stored as one salted SHA-256");
}

TEST(AuthzSeed, Finding769_AnInterruptedOwnerSeedIsResumed) {
    TempDir dir;
    Node node{config_for(dir)};
    ASSERT_TRUE(node.start());
    // What a crash between creating the owner and issuing its token leaves.
    ASSERT_TRUE(node.create_user("owner"));
    const auto boot = node.bootstrap_owner(kOwnerSecret);
    const bool locked_out = node.authenticate_token(kOwnerSecret) == nullptr;
    node.stop();
    // The plain assertion, once fixed:
    //   EXPECT_TRUE(boot.performed); EXPECT_FALSE(locked_out);
    KNOWN_DEFECT(769, locked_out,
                 "an owner left without a token is never re-seeded (" + boot.reason +
                     ")");
}

TEST_F(AuthzNode, Finding769_RefusedPrivilegedSqlIsAudited) {
    const std::string reader = user_with_roles(node, "rita", {"reader"});
    const auto before = node.audit_events().size();
    EXPECT_TRUE(refused(as(reader).run("CREATE TOKEN FOR owner;", "")));
    EXPECT_TRUE(refused(as(reader).run("DROP TABLE app.people;", "")));
    std::size_t refusals = 0;
    const auto events = node.audit_events();
    for (std::size_t i = before; i < events.size(); ++i) {
        if (events[i]->action() == AuditAction::DataChanged && !events[i]->allowed()) {
            ++refusals;
        }
    }
    EXPECT_EQ(refusals, 2u);
}

// ===== the HTTP surface: #762, #766 and #769 through the one auth gate ========

namespace {

struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_request(unsigned short port, const std::string& method,
                          const std::string& target, const std::string& bearer) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }
    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    if (!bearer.empty()) request += "Authorization: Bearer " + bearer + "\r\n";
    request += "Content-Length: 0\r\nConnection: close\r\n\r\n";
    ::send(fd, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(fd, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(fd);

    HttpResponse parsed;
    if (response.size() > 12) parsed.status = std::atoi(response.substr(9, 3).c_str());
    const auto separator = response.find("\r\n\r\n");
    if (separator != std::string::npos) parsed.body = response.substr(separator + 4);
    return parsed;
}

// The request runs on a helper thread while this thread, the owning loop,
// drains the server (the gate and the handler both marshal onto it).
HttpResponse drive(zetta::api::ApiServer& server, const std::string& method,
                   const std::string& target, const std::string& bearer = {}) {
    HttpResponse response;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        response = http_request(server.port(), method, target, bearer);
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();
    return response;
}

struct AuthzHttp : AuthzNode {
    std::unique_ptr<zetta::api::ApiServer> server;

    void SetUp() override {
        AuthzNode::SetUp();
        if (HasFatalFailure()) return;
        zetta::api::ServerOptions options;
        options.port = 0;
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
    }
    void TearDown() override {
        if (server) server->stop();
        AuthzNode::TearDown();
    }
};

}  // namespace

// #762 part 1 (fixed, #799): no credential, a wrong one, and a legacy AccessKey
// id are all refused; only a token that resolves to a user gets in. #769 item 2
// (fixed, #799/#806): there is no shared secret left to compare, constant-time or
// not — every bearer goes through the token store.
TEST_F(AuthzHttp, Finding762_OnlyATokenThatResolvesToAUserGetsIn) {
    auto service = node.create_principal("svc");
    ASSERT_TRUE(service);
    auto key = service->create_key(zetta::governance::unrestricted_scope());
    ASSERT_TRUE(key);

    EXPECT_EQ(drive(*server, "GET", "/v1/server_info").status, 401);
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", "not-a-token").status, 401);
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", key->id()).status, 401);
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", kOwnerSecret).status, 200);
}

// #766: a user whose only right is one tool still reads every tool's spec. (The
// OpenAPI document and llms.txt were made public on purpose, before the gate,
// so they are not probed; /v1/specs is behind the gate.)
TEST_F(AuthzHttp, Finding766_SpecsAreFilteredByTheCallersTools) {
    node.create_role("one_tool");
    ASSERT_TRUE(node.grant_scoped_permissions("one_tool", {"mcp:call"},
                                              Resource::tool("server_info")));
    const std::string tina = user_with_roles(node, "tina", {"one_tool"});
    const auto issued = node.create_token(tina, TokenType::Api);
    ASSERT_TRUE(issued.token);

    const HttpResponse specs = drive(*server, "GET", "/v1/specs", issued.secret);
    const bool lists_other_tools =
        specs.status == 200 &&
        specs.body.find("\"list_projects\"") != std::string::npos;
    // The plain assertion, once fixed: EXPECT_FALSE(lists_other_tools).
    KNOWN_DEFECT(766, lists_other_tools,
                 "a user granted only server_info reads list_projects' spec at "
                 "/v1/specs");
}

// ===== #763 / #764: identity across the cluster ===============================

namespace {

template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

// Node A is the existing cluster with its owner; node B is a fresh node about
// to join it. The clusters are started by the test, so it can act on B first.
struct TwoNodes {
    TempDir dir_a;
    TempDir dir_b;
    std::string id_a = zetta::util::new_uuid();
    std::string id_b = zetta::util::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    std::unique_ptr<Cluster> cluster_a;
    std::unique_ptr<Cluster> cluster_b;
    unsigned short port_a;
    unsigned short port_b;

    TwoNodes(unsigned short first_port, unsigned short second_port)
        : port_a(first_port), port_b(second_port) {
        a.start();
        b.start();
    }
    ~TwoNodes() {
        if (cluster_b) cluster_b->stop();
        if (cluster_a) cluster_a->stop();
        a.stop();
        b.stop();
    }
    void join() {
        ClusterConfig cfg_a;
        cfg_a.listen_host = "127.0.0.1";
        cfg_a.listen_port = port_a;
        cfg_a.port_range = 1;
        cluster_a = std::make_unique<Cluster>(id_a, cfg_a, &repl_a);
        cluster_a->start();
        ClusterConfig cfg_b;
        cfg_b.listen_host = "127.0.0.1";
        cfg_b.listen_port = port_b;
        cfg_b.port_range = 1;
        cfg_b.seeds = {"127.0.0.1:" + std::to_string(port_a)};
        cluster_b = std::make_unique<Cluster>(id_b, cfg_b, &repl_b);
        cluster_b->start();
    }
};

// The internal CRDT key scheme (src/core/catalog.hpp) is not on the tests'
// include path; mirror the keys this file writes, as the replication tests do.
std::string role_key(const std::string& id) { return "role:" + id; }
std::string user_key(const std::string& id) { return "user:" + id; }
std::string user_role_key(const std::string& id) { return "user_role:" + id; }
std::string token_key(const std::string& id) { return "token:" + id; }

std::size_t owners(const Node& node) {
    std::size_t count = 0;
    for (const auto& user : node.users()) {
        if (user->name() == "owner") ++count;
    }
    return count;
}

}  // namespace

// #763: a joining node checks its own, still-empty store before it has synced,
// and seeds a second owner that spreads to the cluster. The order modelled is
// main.cpp's: bootstrap_owner, then cluster start. A fix that moves seeding after
// the state exchange changes that order; model the new one here when it lands.
TEST(AuthzCluster, Finding763_AJoiningNodeDoesNotMintASecondOwner) {
    TwoNodes nodes{43780, 43781};
    ASSERT_TRUE(nodes.a.bootstrap_owner("cluster-owner-secret").performed);
    const auto joiner = nodes.b.bootstrap_owner("joiner-secret");
    nodes.join();
    ASSERT_TRUE(converge(nodes.a, nodes.b, [&] {
        return nodes.b.authenticate_token("cluster-owner-secret") != nullptr &&
               nodes.repl_a.version() == nodes.repl_b.version();
    }));
    const bool second_owner = owners(nodes.a) > 1 ||
                              nodes.a.authenticate_token("joiner-secret") != nullptr;
    // The plain assertion, once fixed:
    //   EXPECT_EQ(owners(nodes.a), 1u);
    //   EXPECT_EQ(nodes.a.authenticate_token("joiner-secret"), nullptr);
    (void)joiner;
    KNOWN_DEFECT(763, second_owner,
                 "a joining node's --admin-token became a second owner credential "
                 "on the existing node");
}

// #764: identity records carry no proof of origin. A peer writes a user, an
// admin role, the assignment and a token straight into the replicated catalog
// (no SQL, no Node call, no issuer) and the existing node honors the token.
TEST(AuthzCluster, Finding764_AnUnsignedReplicatedCredentialIsNotHonored) {
    TwoNodes nodes{43790, 43791};
    ASSERT_TRUE(nodes.a.bootstrap_owner("cluster-owner-secret").performed);

    // Forge the records on a detached node, then write them raw on B.
    TempDir forge_dir;
    Node forge{config_for(forge_dir)};
    ASSERT_TRUE(forge.start());
    auto role = forge.create_builtin_role("admin");
    auto user = forge.create_user("mallory");
    ASSERT_TRUE(role && user);
    ASSERT_TRUE(forge.assign_role(user->id(), role->id()));
    const auto issued = forge.create_token(user->id(), TokenType::Access);
    ASSERT_TRUE(issued.token);
    const std::string token_id = issued.token->id();
    nodes.repl_b.set(role_key(role->id()), role->metadata());
    nodes.repl_b.set(user_key(user->id()), user->metadata());
    for (const auto& assignment : forge.user_roles()) {
        nodes.repl_b.set(user_role_key(assignment->id()),
                         assignment->metadata());
    }
    nodes.repl_b.set(token_key(token_id), issued.token->metadata());
    forge.stop();

    nodes.join();
    ASSERT_TRUE(converge(nodes.a, nodes.b, [&] {
        return nodes.b.authenticate_token("cluster-owner-secret") != nullptr &&
               nodes.repl_a.version() == nodes.repl_b.version();
    }));
    auto honored = nodes.a.authenticate_token(issued.secret);
    const bool admin = honored && nodes.a
                                      .authorize_user(honored->id(),
                                                      Operation{CapabilityDomain::Data,
                                                                "write"})
                                      .allowed;
    // The plain assertion, once fixed:
    //   EXPECT_EQ(nodes.a.authenticate_token(issued.secret), nullptr);
    KNOWN_DEFECT(764, honored != nullptr,
                 std::string("a token written raw into the replicated catalog by a "
                             "peer authenticates on another node") +
                     (admin ? " as an admin" : ""));
}
