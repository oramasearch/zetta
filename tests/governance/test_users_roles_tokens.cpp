#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/governance/token_secret.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::governance::TokenType;
using zetta::governance::UserStatus;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// Tick both nodes until `done` holds (or we give up) — the standard
// capture -> gossip -> merge -> reconcile convergence loop (patient).
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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

// ===== token-secret crypto: the hashing primitive ============================

TEST(TokenSecret, HashIsSaltedDeterministicAndConstantTimeVerified) {
    const std::string secret = zetta::governance::generate_token_secret();
    const std::string salt = zetta::governance::generate_token_salt();
    ASSERT_FALSE(secret.empty());
    ASSERT_FALSE(salt.empty());
    EXPECT_NE(secret, salt);

    const std::string hash = zetta::governance::hash_token_secret(salt, secret);
    ASSERT_EQ(hash.size(), 64u);  // hex SHA-256
    EXPECT_NE(hash, secret);      // never the plaintext

    // Deterministic: same (salt, secret) -> same hash; a different salt diverges.
    EXPECT_EQ(hash, zetta::governance::hash_token_secret(salt, secret));
    const std::string other_salt = zetta::governance::generate_token_salt();
    EXPECT_NE(hash, zetta::governance::hash_token_secret(other_salt, secret));

    // Constant-time compare: equal iff identical.
    EXPECT_TRUE(zetta::governance::constant_time_equal(hash, hash));
    EXPECT_FALSE(zetta::governance::constant_time_equal(hash, hash + "0"));
    EXPECT_FALSE(zetta::governance::constant_time_equal(
        hash, zetta::governance::hash_token_secret(salt, secret + "x")));
}

TEST(TokenSecret, GeneratedSecretsAreUnique) {
    const std::string a = zetta::governance::generate_token_secret();
    const std::string b = zetta::governance::generate_token_secret();
    EXPECT_NE(a, b);  // CSPRNG — collisions are astronomically unlikely
}

TEST(TokenSecret, KdfFormIsSelfDescribingSaltedAndVerified) {
    using zetta::governance::is_kdf_token_hash;
    using zetta::governance::kdf_hash_token_secret;
    using zetta::governance::verify_token_secret;
    const std::string salt = zetta::governance::generate_token_salt();
    const std::string secret = "a-person-chose-this";
    // A small work factor keeps the test fast; the stored form carries it.
    const std::string stored = kdf_hash_token_secret(salt, secret, 1000);
    ASSERT_TRUE(stored.starts_with("pbkdf2-sha256$1000$")) << stored;
    EXPECT_EQ(stored.size(), std::string("pbkdf2-sha256$1000$").size() + 64);
    EXPECT_TRUE(is_kdf_token_hash(stored));
    EXPECT_EQ(stored, kdf_hash_token_secret(salt, secret, 1000));
    EXPECT_NE(stored, kdf_hash_token_secret(salt, secret, 1001));
    EXPECT_NE(stored, kdf_hash_token_secret(zetta::governance::generate_token_salt(),
                                            secret, 1000));
    // It is not the single SHA-256 a stolen store could be guessed against.
    EXPECT_EQ(stored.find(zetta::governance::hash_token_secret(salt, secret)),
              std::string::npos);

    EXPECT_TRUE(verify_token_secret(stored, salt, secret));
    EXPECT_FALSE(verify_token_secret(stored, salt, secret + "x"));
    EXPECT_FALSE(verify_token_secret(stored, salt + "0", secret));

    // A tampered or malformed stored form never matches.
    std::string lowered = stored;
    lowered.replace(std::string("pbkdf2-sha256$").size(), 4, "0999");
    EXPECT_FALSE(verify_token_secret(lowered, salt, secret));
    EXPECT_FALSE(verify_token_secret("pbkdf2-sha256$", salt, secret));
    EXPECT_FALSE(verify_token_secret("pbkdf2-sha256$0$" + std::string(64, 'a'), salt, secret));
    EXPECT_FALSE(verify_token_secret("pbkdf2-sha256$x$" + std::string(64, 'a'), salt, secret));
    EXPECT_FALSE(verify_token_secret("pbkdf2-sha256$99999999$" + std::string(64, 'a'),
                                     salt, secret));
    EXPECT_FALSE(verify_token_secret(stored.substr(0, stored.size() - 1), salt, secret));

    // The plain 64-hex form is still single SHA-256 (generated secrets, and owner
    // tokens seeded before #769).
    const std::string plain = zetta::governance::hash_token_secret(salt, secret);
    EXPECT_FALSE(is_kdf_token_hash(plain));
    EXPECT_TRUE(verify_token_secret(plain, salt, secret));
    EXPECT_FALSE(verify_token_secret(plain, salt, secret + "x"));
}

TEST(TokenSecret, KdfTokenKeepsAMemoryOnlyVerifierAfterItsFirstCheck) {
    using zetta::governance::Token;
    using zetta::governance::TokenValue;
    TempDir dir;
    const std::string secret = "owner-secret";
    TokenValue value;
    value.user_id = "user";
    value.type = TokenType::Access;
    value.salt = zetta::governance::generate_token_salt();
    value.hash = zetta::governance::kdf_hash_token_secret(value.salt, secret, 1000);
    Token token{"id", value, dir.path() / "id"};
    const std::string now = "2026-10-20T00:00:00.000Z";
    ASSERT_TRUE(token.uses_kdf());
    EXPECT_FALSE(token.kdf_verified());
    // Without permission to derive, an unverified KDF token never matches.
    EXPECT_FALSE(token.validates(secret, now, /*allow_kdf=*/false));
    EXPECT_FALSE(token.validates("wrong", now));
    EXPECT_FALSE(token.kdf_verified());
    EXPECT_TRUE(token.validates(secret, now));
    EXPECT_TRUE(token.kdf_verified());
    // Verified: the cheap check answers both ways.
    EXPECT_TRUE(token.validates(secret, now, /*allow_kdf=*/false));
    EXPECT_FALSE(token.validates("wrong", now, /*allow_kdf=*/false));
    // The verifier is never persisted.
    token.persist_meta();
    EXPECT_EQ(read_file(dir.path() / "id" / "token.json")
                  .find(zetta::governance::hash_token_secret(value.salt, secret)),
              std::string::npos);
    // A new secret (a replicated change of hash) drops the old verifier.
    TokenValue rotated = value;
    rotated.salt = zetta::governance::generate_token_salt();
    rotated.hash = zetta::governance::kdf_hash_token_secret(rotated.salt, "next", 1000);
    token.set_value(rotated);
    EXPECT_FALSE(token.kdf_verified());
    EXPECT_FALSE(token.validates(secret, now));
    EXPECT_TRUE(token.validates("next", now));
}

// ===== built-in roles ========================================================

TEST(BuiltinRoles, AdminIsUnrestrictedWriterAndReaderNarrow) {
    const auto admin = zetta::governance::builtin_role_permissions("admin");
    ASSERT_TRUE(admin);
    EXPECT_TRUE(admin->scope.unrestricted);
    const std::string now = "2026-06-25T00:00:00.000Z";
    EXPECT_TRUE(admin->capabilities.permits({CapabilityDomain::Data, "write"}, now));
    EXPECT_TRUE(admin->capabilities.permits({CapabilityDomain::Api, "manage_users"}, now));

    const auto reader = zetta::governance::builtin_role_permissions("reader");
    ASSERT_TRUE(reader);
    EXPECT_TRUE(reader->capabilities.permits({CapabilityDomain::Data, "read"}, now));
    EXPECT_FALSE(reader->capabilities.permits({CapabilityDomain::Data, "write"}, now));

    EXPECT_FALSE(zetta::governance::builtin_role_permissions("nope"));
}

// ===== users / roles / tokens store: create + token validation ===============

TEST(CredentialStore, CreateUserTokenValidatesAndSecretIsHashed) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();

    auto user = node.create_user("analyst");
    ASSERT_TRUE(user);
    EXPECT_EQ(node.create_user("analyst"), nullptr);  // duplicate name rejected
    EXPECT_EQ(node.create_user(""), nullptr);         // empty name rejected

    Node::TokenIssue issue = node.create_token(user->id(), TokenType::Api);
    ASSERT_TRUE(issue.token);
    ASSERT_FALSE(issue.secret.empty());

    // The plaintext validates as the owning user; a wrong secret does not.
    auto authed = node.authenticate_token(issue.secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->id(), user->id());
    EXPECT_EQ(node.authenticate_token(issue.secret + "tamper"), nullptr);
    EXPECT_EQ(node.authenticate_token(""), nullptr);

    // Hashed at rest: token.json stores a hash + salt, NOT the plaintext.
    const std::string on_disk = read_file(issue.token->dir() / "token.json");
    EXPECT_EQ(on_disk.find(issue.secret), std::string::npos)
        << "the plaintext secret must never touch disk";
    EXPECT_NE(on_disk.find("\"hash\""), std::string::npos);
    EXPECT_NE(on_disk.find("\"salt\""), std::string::npos);

    // Revocation stops validation.
    ASSERT_TRUE(node.revoke_token(issue.token->id()));
    EXPECT_EQ(node.authenticate_token(issue.secret), nullptr);
    node.stop();
}

// #1417 (LSR-244): an issued bearer names its token, so authentication
// verifies only that token. Another token's id on this secret, or this id on a
// wrong secret, is refused; an unprefixed bearer (an operator-chosen owner
// secret) still authenticates through the scanning path and is then found by
// its digest.
TEST(AuthenticateToken, IssuedBearersNameTheirTokenAndLegacyBearersStillWork) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner("operator-chosen-owner-secret").performed);
    auto first_user = node.create_user("first");
    auto second_user = node.create_user("second");
    ASSERT_TRUE(first_user && second_user);
    const Node::TokenIssue first = node.create_token(first_user->id(), TokenType::Api);
    const Node::TokenIssue second = node.create_token(second_user->id(), TokenType::Api);
    ASSERT_TRUE(first.token && second.token);

    const std::string prefix = std::string(zetta::governance::kTokenLookupPrefix) +
                               first.token->id() + "_";
    ASSERT_TRUE(first.secret.starts_with(prefix)) << first.secret;
    const auto named = zetta::governance::token_lookup_id(first.secret);
    ASSERT_TRUE(named);
    EXPECT_EQ(*named, first.token->id());
    EXPECT_FALSE(zetta::governance::token_lookup_id("zt1_short"));
    EXPECT_FALSE(zetta::governance::token_lookup_id(std::string(64, 'a')));

    auto authed = node.authenticate_token(first.secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->id(), first_user->id());

    // The id is bound into the hashed secret: moving it onto another token's
    // random part, or keeping it with a wrong one, is refused.
    const std::string first_random = first.secret.substr(prefix.size());
    EXPECT_FALSE(node.authenticate_token(std::string(zetta::governance::kTokenLookupPrefix) +
                                         second.token->id() + "_" + first_random));
    EXPECT_FALSE(node.authenticate_token(prefix + std::string(64, '0')));
    // An unknown id costs no scan and resolves nothing.
    EXPECT_FALSE(node.authenticate_token(std::string(zetta::governance::kTokenLookupPrefix) +
                                         zetta::util::new_uuid() + "_" + first_random));

    // The operator-chosen owner secret has no prefix: the scanning path, then
    // the memory-only digest lookup, both answer the owner.
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto owner = node.authenticate_token("operator-chosen-owner-secret");
        ASSERT_TRUE(owner) << attempt;
        EXPECT_EQ(owner->name(), "owner");
    }
    // A revoked token is refused on either path.
    ASSERT_TRUE(node.revoke_token(first.token->id()));
    EXPECT_FALSE(node.authenticate_token(first.secret));
    node.stop();
}

TEST(CredentialStore, DisabledUserTokensStopValidating) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    auto user = node.create_user("temp");
    ASSERT_TRUE(user);
    Node::TokenIssue issue = node.create_token(user->id(), TokenType::Access);
    ASSERT_TRUE(issue.token);
    EXPECT_TRUE(node.authenticate_token(issue.secret));
    ASSERT_TRUE(node.set_user_status("temp", UserStatus::Disabled));
    EXPECT_EQ(node.authenticate_token(issue.secret), nullptr);
    node.stop();
}

TEST(CredentialStore, ExpiredTokenDoesNotValidate) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    auto user = node.create_user("ttl");
    ASSERT_TRUE(user);
    // A TTL already in the past — the token is born expired.
    Node::TokenIssue issue =
        node.create_token(user->id(), TokenType::Api, "2000-01-01T00:00:00.000Z");
    ASSERT_TRUE(issue.token);
    EXPECT_EQ(node.authenticate_token(issue.secret), nullptr);
    node.stop();
}

TEST(CredentialStore, AssignRoleIsIdempotentAndListsForUser) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    auto user = node.create_user("u");
    ASSERT_TRUE(user);
    auto role = node.create_builtin_role("writer");
    ASSERT_TRUE(role);
    EXPECT_EQ(node.create_builtin_role("writer"), nullptr);  // name clash
    EXPECT_EQ(node.create_builtin_role("bogus"), nullptr);   // unknown built-in
    auto assignment = node.assign_role(user->id(), role->id());
    ASSERT_TRUE(assignment);
    EXPECT_EQ(node.assign_role(user->id(), role->id()), assignment);  // idempotent
    EXPECT_EQ(node.user_role_count(), 1u);
    const auto role_ids = node.roles_for_user(user->id());
    ASSERT_EQ(role_ids.size(), 1u);
    EXPECT_EQ(role_ids.front(), role->id());
    node.stop();
}

// ===== bootstrap owner (#733) ================================================

TEST(BootstrapOwner, SeedsOwnerAdminRoleAndTokenWhenEmpty) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    ASSERT_EQ(node.user_count(), 0u);

    const std::string admin_secret = "s3cr3t-admin-token-value";
    const Node::BootstrapResult boot = node.bootstrap_owner(admin_secret);
    ASSERT_TRUE(boot.performed);
    EXPECT_EQ(boot.user_name, "owner");

    // The owner exists, has the admin role, and its access token authenticates.
    auto owner = node.find_user("owner");
    ASSERT_TRUE(owner);
    EXPECT_EQ(boot.user_id, owner->id());
    ASSERT_TRUE(node.find_role("admin"));
    const auto roles = node.roles_for_user(owner->id());
    ASSERT_EQ(roles.size(), 1u);
    EXPECT_EQ(node.find_role_by_id(roles.front())->name(), "admin");

    auto authed = node.authenticate_token(admin_secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->id(), owner->id());

    // Idempotent: a second call is a no-op (store no longer empty).
    const Node::BootstrapResult again = node.bootstrap_owner(admin_secret);
    EXPECT_FALSE(again.performed);
    EXPECT_EQ(node.user_count(), 1u);
    node.stop();
}

TEST(BootstrapOwner, NoOpWithoutASecretOrOnNonEmptyStore) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    // No secret -> no owner (never mint an unauthenticatable owner).
    EXPECT_FALSE(node.bootstrap_owner("").performed);
    EXPECT_EQ(node.user_count(), 0u);
    // Non-empty store -> no-op.
    ASSERT_TRUE(node.create_user("someone"));
    EXPECT_FALSE(node.bootstrap_owner("secret").performed);
    node.stop();
}

TEST(BootstrapOwner, SurvivesRestartViaDisk) {
    TempDir dir;
    const std::string admin_secret = "restart-me";
    {
        Node node{config_for(dir), nullptr};
        node.start();
        ASSERT_TRUE(node.bootstrap_owner(admin_secret).performed);
        node.stop();
    }
    // A fresh Node over the same data_dir reloads the owner + token from disk.
    Node reloaded{config_for(dir), nullptr};
    reloaded.start();
    ASSERT_TRUE(reloaded.find_user("owner"));
    auto authed = reloaded.authenticate_token(admin_secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->name(), "owner");
    // And bootstrap is now a no-op (the store is seeded).
    EXPECT_FALSE(reloaded.bootstrap_owner(admin_secret).performed);
    reloaded.stop();
}

TEST(BootstrapOwner, StoresTheOwnerTokenThroughTheKdf) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    const std::string admin_secret = "hunter2";  // the kind of secret a person picks
    const Node::BootstrapResult boot = node.bootstrap_owner(admin_secret);
    ASSERT_TRUE(boot.performed);
    ASSERT_EQ(node.token_count(), 1u);
    const auto token = node.tokens().front();
    const std::string& hash = token->value().hash;
    EXPECT_TRUE(hash.starts_with(
        "pbkdf2-sha256$" + std::to_string(zetta::governance::kTokenKdfIterations) + "$"))
        << hash;
    // Nothing on disk is the fast SHA-256 a stolen store could be brute-forced at.
    const std::string fast =
        zetta::governance::hash_token_secret(token->value().salt, admin_secret);
    EXPECT_EQ(read_file(token->dir() / "token.json").find(fast), std::string::npos);
    // Bootstrap verified the secret while it had it, so the owner's requests are cheap.
    EXPECT_TRUE(token->kdf_verified());
    EXPECT_TRUE(node.authenticate_token(admin_secret));
    EXPECT_FALSE(node.authenticate_token("hunter3"));
    node.stop();
}

TEST(BootstrapOwner, ResumesASeedCutShortInsteadOfLockingTheNodeOut) {
    TempDir dir;
    const std::string admin_secret = "resume-me";
    {
        // What a crash between creating the owner and writing its token leaves.
        Node node{config_for(dir), nullptr};
        node.start();
        ASSERT_TRUE(node.create_user("owner"));
        ASSERT_EQ(node.token_count(), 0u);
        node.stop();
    }
    Node node{config_for(dir), nullptr};
    node.start();
    const Node::BootstrapResult boot = node.bootstrap_owner(admin_secret);
    ASSERT_TRUE(boot.performed) << boot.reason;
    EXPECT_EQ(node.user_count(), 1u);
    auto owner = node.find_user("owner");
    ASSERT_TRUE(owner);
    EXPECT_EQ(boot.user_id, owner->id());
    const auto roles = node.roles_for_user(owner->id());
    ASSERT_EQ(roles.size(), 1u);
    EXPECT_EQ(node.find_role_by_id(roles.front())->name(), "admin");
    auto authed = node.authenticate_token(admin_secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->id(), owner->id());
    // Seeded now: a further call is a no-op and mints no second token.
    EXPECT_FALSE(node.bootstrap_owner(admin_secret).performed);
    EXPECT_EQ(node.token_count(), 1u);
    node.stop();
}

TEST(BootstrapOwner, DoesNotResumeWhenTheStoreHoldsOtherUsersOrTheOwnerHasAToken) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    ASSERT_TRUE(node.create_user("owner"));
    ASSERT_TRUE(node.create_user("someone"));
    EXPECT_FALSE(node.bootstrap_owner("secret").performed);
    EXPECT_EQ(node.token_count(), 0u);
    node.stop();

    TempDir other;
    Node seeded{config_for(other), nullptr};
    seeded.start();
    auto owner = seeded.create_user("owner");
    ASSERT_TRUE(owner);
    ASSERT_TRUE(seeded.create_token(owner->id(), TokenType::Access).token);
    EXPECT_FALSE(seeded.bootstrap_owner("secret").performed);
    EXPECT_EQ(seeded.token_count(), 1u);
    EXPECT_FALSE(seeded.authenticate_token("secret"));
    seeded.stop();
}

TEST(AuthenticateToken, UnverifiedKdfChecksAreBudgetedAndCheapTokensAreNot) {
    TempDir dir;
    const std::string admin_secret = "budgeted-owner";
    std::string api_secret;
    {
        Node node{config_for(dir), nullptr};
        node.start();
        ASSERT_TRUE(node.bootstrap_owner(admin_secret).performed);
        auto user = node.create_user("api-user");
        ASSERT_TRUE(user);
        Node::TokenIssue issue = node.create_token(user->id(), TokenType::Api);
        ASSERT_TRUE(issue.token);
        api_secret = issue.secret;
        node.stop();
    }
    // Restarted without the owner's secret: its token is not verified yet.
    Node node{config_for(dir), nullptr};
    node.start();
    // Wrong bearers spend the burst of slow checks. A check that runs PBKDF2
    // takes milliseconds; once the budget is spent a wrong bearer is refused at
    // once, which is how the loop knows it is exhausted (a refill may land while
    // the slow checks run, so the count is not fixed).
    bool exhausted = false;
    for (int attempt = 0; attempt < 4 * Node::kColdKdfBurst && !exhausted; ++attempt) {
        const auto started = std::chrono::steady_clock::now();
        EXPECT_FALSE(node.authenticate_token("wrong-" + std::to_string(attempt)));
        exhausted = std::chrono::steady_clock::now() - started <
                    std::chrono::microseconds(500);
    }
    ASSERT_TRUE(exhausted);
    // ...after which even the right secret is refused until the budget refills,
    EXPECT_FALSE(node.authenticate_token(admin_secret));
    // while a generated (SHA-256) token is never held back by it.
    auto api_user = node.authenticate_token(api_secret);
    ASSERT_TRUE(api_user);
    EXPECT_EQ(api_user->name(), "api-user");

    std::this_thread::sleep_for(Node::kColdKdfRefill + std::chrono::milliseconds(100));
    auto owner = node.authenticate_token(admin_secret);
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner->name(), "owner");
    // Verified now: wrong bearers no longer reach the slow pass, and the owner
    // keeps authenticating however many there were.
    for (int attempt = 0; attempt < 2 * Node::kColdKdfBurst; ++attempt) {
        EXPECT_FALSE(node.authenticate_token("wrong-again-" + std::to_string(attempt)));
    }
    EXPECT_TRUE(node.authenticate_token(admin_secret));
    node.stop();
}

// ===== system.* read views: no secret leakage ================================

TEST(SystemViews, UsersRolesTokensQueryableAndTokensHideSecrets) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    auto user = node.create_user("viewer");
    ASSERT_TRUE(user);
    auto role = node.create_builtin_role("reader");
    ASSERT_TRUE(role);
    node.assign_role(user->id(), role->id());
    Node::TokenIssue issue = node.create_token(user->id(), TokenType::Api);
    ASSERT_TRUE(issue.token);

    QueryService svc{node};

    auto users = svc.run("SELECT * FROM system.users;", "");
    ASSERT_TRUE(users.ok()) << users.error;
    ASSERT_EQ(users.rows.size(), 1u);
    EXPECT_EQ(users.rows[0]["name"], "viewer");
    EXPECT_EQ(users.rows[0]["roles"], "reader");

    auto roles = svc.run("SELECT * FROM system.roles;", "");
    ASSERT_TRUE(roles.ok()) << roles.error;
    ASSERT_EQ(roles.rows.size(), 1u);
    EXPECT_EQ(roles.rows[0]["name"], "reader");

    auto tokens = svc.run("SELECT * FROM system.tokens;", "");
    ASSERT_TRUE(tokens.ok()) << tokens.error;
    ASSERT_EQ(tokens.rows.size(), 1u);
    const nlohmann::json& row = tokens.rows[0];
    EXPECT_EQ(row["token_id"], issue.token->id());
    EXPECT_EQ(row["user"], "viewer");
    EXPECT_EQ(row["revoked"], false);
    // The critical guarantee: NO secret material in the view.
    EXPECT_FALSE(row.contains("hash"));
    EXPECT_FALSE(row.contains("salt"));
    EXPECT_FALSE(row.contains("secret"));
    // And the serialized view text carries neither the plaintext nor the hash.
    const std::string dump = nlohmann::json(tokens.rows).dump();
    EXPECT_EQ(dump.find(issue.secret), std::string::npos);
    EXPECT_EQ(dump.find(issue.token->value().hash), std::string::npos);
    node.stop();
}

// ===== SQL issuance surface: create user + token end to end ==================

TEST(SqlIssuance, CreateUserAndTokenReturnsPlaintextOnce) {
    TempDir dir;
    Node node{config_for(dir), nullptr};
    node.start();
    QueryService svc{node};

    auto created = svc.run("CREATE USER dana;", "");
    ASSERT_TRUE(created.ok()) << created.error;
    ASSERT_TRUE(node.find_user("dana"));

    auto issued = svc.run("CREATE TOKEN FOR dana TYPE access;", "");
    ASSERT_TRUE(issued.ok()) << issued.error;
    ASSERT_EQ(issued.rows.size(), 1u);
    const std::string secret = issued.rows[0]["secret"].get<std::string>();
    ASSERT_FALSE(secret.empty());
    EXPECT_EQ(issued.rows[0]["type"], "access");

    // The plaintext returned once authenticates as dana.
    auto authed = node.authenticate_token(secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->name(), "dana");

    // Errors: unknown user, unknown type, duplicate user.
    EXPECT_FALSE(svc.run("CREATE TOKEN FOR ghost TYPE api;", "").ok());
    EXPECT_FALSE(svc.run("CREATE TOKEN FOR dana TYPE bogus;", "").ok());
    EXPECT_FALSE(svc.run("CREATE USER dana;", "").ok());
    node.stop();
}

// ===== replication: users / roles / tokens converge across peers =============

TEST(CredentialStore, ReplicatesUsersRolesTokensToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43330;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43331;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43330"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Build the whole credential graph on node A only.
    auto user = a.create_user("carol");
    ASSERT_TRUE(user);
    auto role = a.create_builtin_role("writer");
    ASSERT_TRUE(role);
    a.assign_role(user->id(), role->id());
    Node::TokenIssue issue = a.create_token(user->id(), TokenType::Access);
    ASSERT_TRUE(issue.token);
    const std::string user_id = user->id();
    const std::string token_id = issue.token->id();

    // Node B converges to the same user + role + token + assignment.
    const bool converged = converge(a, b, [&] {
        return b.find_user("carol") && b.find_role("writer") &&
               b.find_token_by_id(token_id) &&
               b.roles_for_user(user_id).size() == 1 &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    // The token authenticates on B (the hash replicated, not just the bytes).
    auto authed = b.authenticate_token(issue.secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->name(), "carol");

    // Revoking on A stops validation on B.
    ASSERT_TRUE(a.revoke_token(token_id));
    const bool revoked = converge(a, b, [&] {
        auto token = b.find_token_by_id(token_id);
        return token && token->revoked();
    });
    ASSERT_TRUE(revoked);
    EXPECT_EQ(b.authenticate_token(issue.secret), nullptr);

    // Dropping the user on A removes the user + its token + assignment on B.
    ASSERT_TRUE(a.drop_user("carol"));
    const bool dropped = converge(a, b, [&] {
        return !b.find_user("carol") && !b.find_token_by_id(token_id) &&
               b.roles_for_user(user_id).empty();
    });
    EXPECT_TRUE(dropped);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

// ===== persistence across restart via the CRDT document ======================

TEST(CredentialStore, PersistsAcrossRestartViaCRDT) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::string token_id;
    std::string secret;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(dir), &replication};
        node.start();
        auto user = node.create_user("persist");
        ASSERT_TRUE(user);
        node.create_builtin_role("reader");
        Node::TokenIssue issue = node.create_token(user->id(), TokenType::Api);
        ASSERT_TRUE(issue.token);
        token_id = issue.token->id();
        secret = issue.secret;
        node.tick();  // persist the document
        node.stop();
    }
    // A brand-new ReplicationManager rebuilds the store from the persisted doc.
    ReplicationManager replication{node_id};
    Node node{config_for(dir), &replication};
    node.start();
    ASSERT_TRUE(node.find_user("persist"));
    ASSERT_TRUE(node.find_role("reader"));
    ASSERT_TRUE(node.find_token_by_id(token_id));
    auto authed = node.authenticate_token(secret);
    ASSERT_TRUE(authed);
    EXPECT_EQ(authed->name(), "persist");
    node.stop();
}
