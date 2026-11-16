// SPDX-License-Identifier: MIT

// Clerk JWT verification (#889). Generates an RSA key in-test, signs an RS256
// JWT, serves the matching JWKS through an injected transport, and asserts the
// full verify path — plus the failure closes (wrong issuer, expired, bad
// signature, wrong key, non-JWT).
#include <gtest/gtest.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/api/clerk_auth.hpp"
#include "zetta/connect/http_transport.hpp"

namespace {

std::string base64url(const unsigned char* data, std::size_t len) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    int acc = 0, bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 6) { bits -= 6; out.push_back(alphabet[(acc >> bits) & 0x3F]); }
    }
    if (bits > 0) out.push_back(alphabet[(acc << (6 - bits)) & 0x3F]);
    return out;
}
std::string base64url(const std::string& s) {
    return base64url(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

// A generated RSA keypair + its JWK (n, e) for the JWKS.
struct TestKey {
    EVP_PKEY* pkey = nullptr;
    std::string n, e;  // base64url
    ~TestKey() { EVP_PKEY_free(pkey); }
};

TestKey make_key() {
    TestKey k;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY_keygen(ctx, &k.pkey);
    EVP_PKEY_CTX_free(ctx);
    BIGNUM* n = nullptr; BIGNUM* e = nullptr;
    EVP_PKEY_get_bn_param(k.pkey, "n", &n);
    EVP_PKEY_get_bn_param(k.pkey, "e", &e);
    std::string nb(BN_num_bytes(n), '\0'), eb(BN_num_bytes(e), '\0');
    BN_bn2bin(n, reinterpret_cast<unsigned char*>(nb.data()));
    BN_bn2bin(e, reinterpret_cast<unsigned char*>(eb.data()));
    k.n = base64url(nb); k.e = base64url(eb);
    BN_free(n); BN_free(e);
    return k;
}

std::string sign_rs256(EVP_PKEY* pkey, const std::string& signing_input) {
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(md, signing_input.data(), signing_input.size());
    std::size_t len = 0;
    EVP_DigestSignFinal(md, nullptr, &len);
    std::string sig(len, '\0');
    EVP_DigestSignFinal(md, reinterpret_cast<unsigned char*>(sig.data()), &len);
    sig.resize(len);
    EVP_MD_CTX_free(md);
    return sig;
}

// The Studio origin the test tokens are issued to, this node's only authorized party (#1423).
const bool studio_party_configured =
    (zetta::api::set_clerk_authorized_parties({"https://studio.example.test"}), true);

std::string make_jwt(EVP_PKEY* pkey, const std::string& kid, const std::string& iss,
                     const std::string& sub, long exp,
                     nlohmann::json extra = nlohmann::json::object()) {
    const std::string header = base64url(nlohmann::json{{"alg", "RS256"}, {"typ", "JWT"}, {"kid", kid}}.dump());
    nlohmann::json claims{{"iss", iss}, {"sub", sub}, {"exp", exp}, {"email", "u@x.com"},
                          {"azp", "https://studio.example.test"}};
    for (const auto& [name, value] : extra.items()) {
        if (value.is_null()) claims.erase(name); else claims[name] = value;
    }
    const std::string payload = base64url(claims.dump());
    const std::string signing_input = header + "." + payload;
    return signing_input + "." + base64url(sign_rs256(pkey, signing_input));
}

// A transport that returns a fixed JWKS body for the .well-known request.
zetta::connect::HttpTransport jwks_transport(const std::string& kid, const TestKey& key) {
    const std::string body = nlohmann::json{{"keys", {{{"kty", "RSA"}, {"kid", kid}, {"n", key.n}, {"e", key.e}, {"use", "sig"}, {"alg", "RS256"}}}}}.dump();
    return [body](const zetta::connect::HttpRequest& req) {
        zetta::connect::HttpResponse res;
        if (req.url.find("/.well-known/jwks.json") != std::string::npos) {
            res.status = 200; res.body = body;
        } else {
            res.status = 404;
        }
        return res;
    };
}

long now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* kIssuer = "https://clerk.example.dev";

TEST(ClerkAuth, VerifiesAValidToken) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey key = make_key();
    const std::string jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    auto claims = zetta::api::verify_clerk_jwt(kIssuer, jwt, jwks_transport("kid-1", key), now());
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->subject, "user_abc");
    EXPECT_EQ(claims->email, "u@x.com");
}

// #1423: a signed token must also carry an expiry and be issued to one of this node's
// own origins; a token minted for another product sharing the Clerk instance is refused.
TEST(ClerkAuth, RequiresAnExpiryAndThisNodesAuthorizedParty) {
    ASSERT_TRUE(studio_party_configured);
    TestKey key = make_key();
    for (const nlohmann::json& changed :
         {nlohmann::json{{"azp", "https://other-product.example.test"}},
          nlohmann::json{{"azp", nullptr}},
          nlohmann::json{{"exp", nullptr}},
          nlohmann::json{{"exp", "tomorrow"}}}) {
        zetta::api::clear_clerk_jwks_cache();
        const std::string jwt =
            make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600, changed);
        EXPECT_FALSE(
            zetta::api::verify_clerk_jwt(kIssuer, jwt, jwks_transport("kid-1", key), now()).has_value())
            << changed.dump();
    }
    zetta::api::clear_clerk_jwks_cache();
    const std::string trailing_slash = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600,
                                                {{"azp", "https://studio.example.test/"}});
    EXPECT_TRUE(zetta::api::verify_clerk_jwt(kIssuer, trailing_slash, jwks_transport("kid-1", key), now())
                    .has_value());
}

TEST(ClerkAuth, RejectsWrongIssuer) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey key = make_key();
    const std::string jwt = make_jwt(key.pkey, "kid-1", "https://evil.dev", "user_abc", now() + 3600);
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt, jwks_transport("kid-1", key), now()).has_value());
}

TEST(ClerkAuth, RejectsExpired) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey key = make_key();
    const std::string jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() - 10);
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt, jwks_transport("kid-1", key), now()).has_value());
}

TEST(ClerkAuth, RejectsTamperedSignature) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey key = make_key();
    std::string jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    // Flip a char just after the second dot — the start of the signature, whose
    // bits are all significant (unlike the last char, whose low bits are padding
    // for a 256-byte RSA sig). This corrupts the decoded signature for real.
    const std::size_t sig_start = jwt.rfind('.') + 1;
    jwt[sig_start] = (jwt[sig_start] == 'A') ? 'B' : 'A';
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt, jwks_transport("kid-1", key), now()).has_value());
}

TEST(ClerkAuth, RejectsUnknownKid) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey key = make_key();
    const std::string jwt = make_jwt(key.pkey, "kid-other", kIssuer, "user_abc", now() + 3600);
    // JWKS only carries kid-1; the token names kid-other → no matching key.
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt, jwks_transport("kid-1", key), now()).has_value());
}

TEST(ClerkAuth, RejectsSignatureFromAnotherKey) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey signer = make_key();
    TestKey published = make_key();  // JWKS publishes a DIFFERENT key than signed with
    const std::string jwt = make_jwt(signer.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt, jwks_transport("kid-1", published), now()).has_value());
}

TEST(ClerkAuth, LooksLikeJwt) {
    EXPECT_TRUE(zetta::api::looks_like_jwt("aaa.bbb.ccc"));
    EXPECT_FALSE(zetta::api::looks_like_jwt("opaque-native-token"));
    EXPECT_FALSE(zetta::api::looks_like_jwt("only.two"));
}

// The legacy entry point shares the async gate's cache, including outages.
// This regression uses only the original public API for negative proof.
TEST(ClerkAuth, FailedFetchesAreCached) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    std::atomic<int> calls{0};
    zetta::connect::HttpTransport failing = [&](const auto&) {
        ++calls;
        return zetta::connect::HttpResponse{503, {}, {}};
    };
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt, failing));
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt, failing));
    EXPECT_EQ(calls, 1);
}

TEST(ClerkAuth, SynchronousDefaultStillUsesTheSharedFailureCache) {
    zetta::api::clear_clerk_jwks_cache();
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    std::atomic<int> calls{0};
    zetta::connect::set_default_http_transport([&](const auto&) {
        ++calls;
        return zetta::connect::HttpResponse{503, {}, {}};
    }, {});
    EXPECT_FALSE(zetta::runtime::block_on(zetta::api::verify_clerk_jwt_async(kIssuer, jwt)));
    EXPECT_FALSE(zetta::api::verify_clerk_jwt(kIssuer, jwt));
    EXPECT_EQ(calls, 1);
    zetta::connect::set_default_http_transport({}, {});
    zetta::api::clear_clerk_jwks_cache();
}

struct CacheClock {
    std::atomic<long> seconds{1};
    auto read() const {
        return std::chrono::steady_clock::time_point(std::chrono::seconds(seconds.load()));
    }
};

zetta::runtime::io_task<bool> attempt(const zetta::api::ClerkJwksCache& cache,
                                     std::string issuer, std::string jwt,
                                     zetta::connect::AsyncHttpTransport transport) {
    auto claims = co_await cache.verify(std::move(issuer), std::move(jwt), std::move(transport));
    co_return {{}, claims.has_value()};
}

TEST(ClerkAuth, ConcurrentFailedMissesCoalesceAndDoNotBlockTheExecutor) {
    zetta::api::ClerkJwksCache cache;
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    std::atomic<int> calls{0};
    std::atomic<bool> ticked{false};
    zetta::connect::AsyncHttpTransport failing = [&](const auto& request)
        -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
        EXPECT_EQ(request.timeout_seconds, 2);
        EXPECT_EQ(request.max_response_bytes, 1024 * 1024);
        ++calls;
        const auto [error] = co_await zetta::runtime::delay(std::chrono::milliseconds(100));
        EXPECT_TRUE(ticked.load());
        co_return {error, zetta::connect::HttpResponse{503, {}, {}}};
    };
    auto tick = [&]() -> zetta::runtime::io_task<bool> {
        const auto [error] = co_await zetta::runtime::delay(std::chrono::milliseconds(10));
        ticked = true;
        co_return {error, false};
    };
    std::vector<zetta::runtime::io_task<bool>> pending;
    for (int index = 0; index < 32; ++index) {
        // A token's slash spelling cannot partition the issuer cache.
        pending.push_back(attempt(cache, std::string(kIssuer) + (index % 2 ? "/" : ""),
                                  jwt, failing));
    }
    pending.push_back(tick());
    auto [error, results] = zetta::runtime::block_on(zetta::runtime::when_all(std::move(pending)));
    EXPECT_FALSE(error);
    for (bool accepted : results) EXPECT_FALSE(accepted);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, failing)));
    EXPECT_EQ(calls, 1);
}

TEST(ClerkAuth, FailureCacheRetriesAndRecoversForEveryUpstreamError) {
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    auto request = zetta::connect::HttpRequest{};
    request.url = "/.well-known/jwks.json";
    const auto success = jwks_transport("kid-1", key)(request);
    // Network errors, status errors, empty/malformed sets and wrong JSON types
    // must all release the flight and install the same failure backoff.
    const std::vector<zetta::connect::HttpResponse> failures{
        {0, {}, "unreachable"}, {503, {}, {}}, {200, "not json", {}},
        {200, R"({"keys":[]})", {}}, {200, R"({"keys":[{"kty":42}]})", {}}};
    for (const auto& failure : failures) {
        CacheClock clock;
        zetta::api::ClerkJwksCache cache([&] { return clock.read(); });
        int calls = 0;
        zetta::connect::AsyncHttpTransport transport = [&](const auto&)
            -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
            ++calls;
            co_return {{}, calls == 1 ? failure : success};
        };
        EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
        EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
        EXPECT_EQ(calls, 1);
        clock.seconds += 5;
        EXPECT_TRUE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
        EXPECT_EQ(calls, 2);
    }
}

TEST(ClerkAuth, SlowFetchHasABoundedDeadlineAndCachesItsFailure) {
    zetta::api::ClerkJwksCache cache;
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    std::atomic<int> calls{0};
    zetta::connect::AsyncHttpTransport stalled = [&](const auto&)
        -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
        ++calls;
        const auto [error] = co_await zetta::runtime::delay(std::chrono::seconds(30));
        co_return {error, zetta::connect::HttpResponse{}};
    };
    const auto began = std::chrono::steady_clock::now();
    std::vector<zetta::runtime::io_task<bool>> pending;
    for (int index = 0; index < 16; ++index)
        pending.push_back(attempt(cache, kIssuer, jwt, stalled));
    auto [error, results] = zetta::runtime::block_on(zetta::runtime::when_all(std::move(pending)));
    EXPECT_FALSE(error);
    for (bool accepted : results) EXPECT_FALSE(accepted);
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(6));
    // Join an existing completion if a waiter's timer won the deadline race.
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, stalled)));
    EXPECT_EQ(calls, 1);
}

TEST(ClerkAuth, FailedRefreshNeverExtendsTheStaleKeyCutoff) {
    CacheClock clock;
    zetta::api::ClerkJwksCache cache([&] { return clock.read(); });
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    auto request = zetta::connect::HttpRequest{};
    request.url = "/.well-known/jwks.json";
    const auto success = jwks_transport("kid-1", key)(request);
    std::atomic<int> calls{0};
    zetta::connect::AsyncHttpTransport transport = [&](const auto&)
        -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
        if (++calls == 1) co_return {{}, success};
        co_return {{}, zetta::connect::HttpResponse{503, {}, {}}};
    };
    ASSERT_TRUE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
    clock.seconds += 3300;
    EXPECT_TRUE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
    EXPECT_TRUE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
    EXPECT_EQ(calls, 2);
    clock.seconds += 300;
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
    EXPECT_EQ(calls, 3);
}

TEST(ClerkAuth, RotationReplacesKeysAndUnknownKidsDoNotTriggerFetches) {
    CacheClock clock;
    zetta::api::ClerkJwksCache cache([&] { return clock.read(); });
    TestKey old_key = make_key(), new_key = make_key();
    const auto old_jwt = make_jwt(old_key.pkey, "old", kIssuer, "user_abc", now() + 3600);
    const auto new_jwt = make_jwt(new_key.pkey, "new", kIssuer, "user_abc", now() + 3600);
    auto request = zetta::connect::HttpRequest{};
    request.url = "/.well-known/jwks.json";
    const auto old_keys = jwks_transport("old", old_key)(request);
    const auto new_keys = jwks_transport("new", new_key)(request);
    int calls = 0;
    zetta::connect::AsyncHttpTransport transport = [&](const auto&)
        -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
        ++calls;
        co_return {{}, calls == 1 ? old_keys : new_keys};
    };
    ASSERT_TRUE(zetta::runtime::block_on(cache.verify(kIssuer, old_jwt, transport)));
    for (int index = 0; index < 10; ++index)
        EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, new_jwt, transport)));
    EXPECT_EQ(calls, 1);
    clock.seconds += 3600;
    EXPECT_TRUE(zetta::runtime::block_on(cache.verify(kIssuer, new_jwt, transport)));
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, old_jwt, transport)));
    EXPECT_EQ(calls, 2);
}

TEST(ClerkAuth, CancellingOneWaiterDoesNotCancelTheSharedFetch) {
    zetta::api::ClerkJwksCache cache;
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    auto request = zetta::connect::HttpRequest{};
    request.url = "/.well-known/jwks.json";
    const auto success = jwks_transport("kid-1", key)(request);
    std::atomic<int> calls{0};
    zetta::runtime::async_event started, release;
    zetta::connect::AsyncHttpTransport transport = [&](const auto&)
        -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
        ++calls;
        started.set();
        const auto [error] = co_await release.wait();
        co_return {error, success};
    };
    auto cancel_follower = [&]() -> zetta::runtime::io_task<bool> {
        // Start after the leader has entered its coalesced fetch.
        const auto [started_error] = co_await started.wait();
        EXPECT_FALSE(started_error);
        auto [error, accepted] = co_await zetta::runtime::timeout(
            attempt(cache, kIssuer, jwt, transport), std::chrono::milliseconds(20));
        EXPECT_TRUE(error);
        EXPECT_FALSE(accepted);
        release.set();
        co_return {{}, false};
    };
    std::vector<zetta::runtime::io_task<bool>> pending;
    pending.push_back(attempt(cache, kIssuer, jwt, transport));
    pending.push_back(cancel_follower());
    auto [error, results] = zetta::runtime::block_on(
        zetta::runtime::when_all(std::move(pending)));
    EXPECT_FALSE(error);
    ASSERT_EQ(results.size(), 2);
    EXPECT_TRUE(results[0]);
    EXPECT_EQ(calls, 1);
}

TEST(ClerkAuth, MalformedTokensNeverFetchAndTransportExceptionsAreCached) {
    zetta::api::ClerkJwksCache cache;
    int calls = 0;
    zetta::connect::AsyncHttpTransport transport = [&](const auto&)
        -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
        ++calls;
        throw std::runtime_error("failed");
        co_return {{}, zetta::connect::HttpResponse{}};
    };
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, "e30.e30.AA", transport)));
    const auto bad_header = base64url(R"({"alg":42})") + ".e30.AA";
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, bad_header, transport)));
    EXPECT_EQ(calls, 0);
    TestKey key = make_key();
    const auto jwt = make_jwt(key.pkey, "kid-1", kIssuer, "user_abc", now() + 3600);
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
    EXPECT_FALSE(zetta::runtime::block_on(cache.verify(kIssuer, jwt, transport)));
    EXPECT_EQ(calls, 1);
}

}  // namespace
