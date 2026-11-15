/*
 * Clerk JWT verification (#889). RS256 over the issuer's JWKS, via OpenSSL. See
 * clerk_auth.hpp for the contract. Non-throwing: every failure path returns
 * nullopt, so the auth gate never unwinds.
 */

#include <algorithm>
#include "zetta/api/clerk_auth.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::api {

namespace {
std::mutex clerk_parties_mutex;
std::vector<std::string> clerk_parties;

bool clerk_party_allowed(std::string party) {
    while (!party.empty() && party.back() == '/') party.pop_back();
    const std::lock_guard lock(clerk_parties_mutex);
    return std::ranges::find(clerk_parties, party) != clerk_parties.end();
}
}  // namespace

void set_clerk_authorized_parties(std::vector<std::string> parties) {
    for (auto& party : parties) {
        while (!party.empty() && party.back() == '/') party.pop_back();
    }
    std::erase_if(parties, [](const std::string& party) { return party.empty(); });
    const std::lock_guard lock(clerk_parties_mutex);
    clerk_parties = std::move(parties);
}


namespace {

// base64url decode (RFC 7515): '-'→'+', '_'→'/', no padding. Returns nullopt on
// any non-alphabet byte, so a malformed segment fails closed.
std::optional<std::string> base64url_decode(std::string_view input) {
    static constexpr int kInvalid = -1;
    auto value_of = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return kInvalid;
    };
    std::string out;
    out.reserve(input.size() * 3 / 4 + 3);
    int accumulator = 0;
    int bits = 0;
    for (unsigned char c : input) {
        const int v = value_of(c);
        if (v == kInvalid) return std::nullopt;
        accumulator = (accumulator << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

// Split "a.b.c" into its three dot-separated parts; nullopt if not exactly three.
std::optional<std::array<std::string_view, 3>> split_jwt(std::string_view token) {
    const auto first = token.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const auto second = token.find('.', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    if (token.find('.', second + 1) != std::string_view::npos) return std::nullopt;
    return std::array<std::string_view, 3>{token.substr(0, first),
                                           token.substr(first + 1, second - first - 1),
                                           token.substr(second + 1)};
}

// One JWK: an RSA public key by kid.
struct JwkKey {
    std::string kid;
    std::string modulus_n;    // base64url
    std::string exponent_e;   // base64url
};

runtime::io_task<connect::HttpResponse> offloaded_transport(
    connect::HttpTransport transport, connect::HttpRequest request);

constexpr std::chrono::minutes kJwksTtl{55};
constexpr std::chrono::minutes kJwksStaleGrace{5};
constexpr std::chrono::seconds kJwksFailureTtl{5};
constexpr std::chrono::seconds kJwksDeadline{2};

std::vector<JwkKey> parse_jwks(const std::string& body) {
    std::vector<JwkKey> keys;
    const nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
    if (doc.is_discarded() || !doc.contains("keys") || !doc["keys"].is_array()) return keys;
    for (const auto& key : doc["keys"]) {
        if (!key.is_object()) continue;
        if (key.value("kty", "") != "RSA") continue;
        JwkKey jwk;
        jwk.kid = key.value("kid", "");
        jwk.modulus_n = key.value("n", "");
        jwk.exponent_e = key.value("e", "");
        if (!jwk.modulus_n.empty() && !jwk.exponent_e.empty()) keys.push_back(std::move(jwk));
    }
    return keys;
}

// Build an OpenSSL EVP_PKEY (RSA public) from base64url n + e, and RS256-verify
// `signed_input` against `signature`. Returns true only on a valid signature.
bool rs256_verify(const JwkKey& key, std::string_view signed_input,
                  const std::string& signature) {
    const auto n_bytes = base64url_decode(key.modulus_n);
    const auto e_bytes = base64url_decode(key.exponent_e);
    if (!n_bytes || !e_bytes) return false;

    BIGNUM* n = BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bytes->data()),
                          static_cast<int>(n_bytes->size()), nullptr);
    BIGNUM* e = BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bytes->data()),
                          static_cast<int>(e_bytes->size()), nullptr);
    if (!n || !e) { BN_free(n); BN_free(e); return false; }

    // OSSL_PARAM path (OpenSSL 3): assemble the public RSA key.
    bool ok = false;
    EVP_PKEY* pkey = nullptr;
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (bld && OSSL_PARAM_BLD_push_BN(bld, "n", n) && OSSL_PARAM_BLD_push_BN(bld, "e", e)) {
        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        if (params && ctx && EVP_PKEY_fromdata_init(ctx) > 0) {
            EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
        }
        EVP_PKEY_CTX_free(ctx);
        OSSL_PARAM_free(params);
    }
    OSSL_PARAM_BLD_free(bld);
    BN_free(n);
    BN_free(e);
    if (!pkey) return false;

    EVP_MD_CTX* md = EVP_MD_CTX_new();
    if (md &&
        EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerify(
            md, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
            reinterpret_cast<const unsigned char*>(signed_input.data()),
            signed_input.size()) == 1) {
        ok = true;
    }
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ok;
}

}  // namespace

bool looks_like_jwt(std::string_view token) {
    return split_jwt(token).has_value();
}

struct ClerkJwksCache::State {
    struct Entry {
        std::vector<JwkKey> keys;
        std::chrono::steady_clock::time_point fetched{};
        std::chrono::steady_clock::time_point retry_at{};
        std::shared_ptr<runtime::async_event> fetching;
    };

    explicit State(Clock read_clock) : clock(std::move(read_clock)) {}

    // Lowest-level handoff between the HTTP executors and the refresh coroutine.
    // No lock spans an await or transport call; cache state never touches Node.
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Entry>, std::less<>> entries;
    Clock clock;

    static runtime::task<> refresh(std::shared_ptr<State> state,
                                    std::shared_ptr<Entry> entry,
                                    std::shared_ptr<runtime::async_event> finished,
                                    std::string issuer,
                                    connect::AsyncHttpTransport transport) {
        std::vector<JwkKey> keys;
        try {
            if (!transport) transport = connect::default_async_http_transport();
            if (!transport) {
                // Preserve callers that installed only the synchronous shape.
                // The real native client always supplies the async shape above.
                if (auto synchronous = connect::default_http_transport()) {
                    transport = [synchronous](const connect::HttpRequest& request) {
                        return offloaded_transport(synchronous, request);
                    };
                }
            }
            if (transport) {
                connect::HttpRequest request;
                request.method = "GET";
                request.url = issuer + "/.well-known/jwks.json";
                request.timeout_seconds = static_cast<int>(kJwksDeadline.count());
                request.max_response_bytes = 1024 * 1024;
                auto [error, response] = co_await runtime::timeout(
                    transport(request), kJwksDeadline);
                if (!error && response.ok()) keys = parse_jwks(response.body);
            }
        } catch (...) {
            // Malformed JWKS field types and transport exceptions are failures,
            // just like HTTP/DNS errors. They must release and cache this flight.
        }
        {
            const std::lock_guard lock(state->mutex);
            const auto now = state->clock();
            if (!keys.empty()) {
                entry->keys = std::move(keys);
                entry->fetched = now;
                entry->retry_at = now + kJwksTtl;
            } else {
                entry->retry_at = now + kJwksFailureTtl;
            }
            entry->fetching.reset();
        }
        finished->set();
    }

    static runtime::task<std::vector<JwkKey>> keys_for(
        std::shared_ptr<State> state, std::string issuer,
        connect::AsyncHttpTransport transport) {
        std::shared_ptr<Entry> entry;
        std::shared_ptr<runtime::async_event> finished;
        bool start = false;
        {
            const std::lock_guard lock(state->mutex);
            auto& slot = state->entries[issuer];
            if (!slot) slot = std::make_shared<Entry>();
            entry = slot;
            const auto now = state->clock();
            if (!entry->keys.empty() && now - entry->fetched < kJwksTtl)
                co_return entry->keys;
            if (!entry->fetching && now >= entry->retry_at) {
                entry->fetching = std::make_shared<runtime::async_event>();
                start = true;
            }
            finished = entry->fetching;
        }
        if (start) {
            // The initiating request owns the refresh, bounded by its own timer.
            // Defer its cancellation so another waiter cannot lose the flight,
            // and server shutdown joins this work with the initiating request.
            co_await runtime::with_stop_token(
                refresh(state, entry, finished, std::move(issuer),
                        std::move(transport)), {});
        } else if (finished) {
            const auto [error] = co_await runtime::timeout(finished->wait(), kJwksDeadline);
            if (error) co_return std::vector<JwkKey>{};
        }
        const std::lock_guard lock(state->mutex);
        if (state->clock() - entry->fetched < kJwksTtl + kJwksStaleGrace)
            co_return entry->keys;
        co_return std::vector<JwkKey>{};
    }

    static runtime::task<std::optional<ClerkClaims>> verify(
        std::shared_ptr<State> state, std::string issuer, std::string token,
        connect::AsyncHttpTransport transport, long now_unix);
};

ClerkJwksCache::ClerkJwksCache(Clock clock)
    : state_(std::make_shared<State>(std::move(clock))) {}

void ClerkJwksCache::clear() {
    const std::lock_guard lock(state_->mutex);
    // Existing flights finish into their original entry, never repopulating a
    // cleared cache or waking waiters for a newer fetch.
    state_->entries.clear();
}

runtime::task<std::optional<ClerkClaims>> ClerkJwksCache::verify(
    std::string issuer, std::string token, connect::AsyncHttpTransport transport,
    long now_unix) const {
    return State::verify(state_, std::move(issuer), std::move(token),
                         std::move(transport), now_unix);
}

runtime::task<std::optional<ClerkClaims>> ClerkJwksCache::State::verify(
    std::shared_ptr<State> state, std::string issuer, std::string token,
    connect::AsyncHttpTransport transport, long now_unix) {
    try {
        if (issuer.empty()) co_return std::nullopt;
        const auto parts = split_jwt(token);
        if (!parts) co_return std::nullopt;

        // Header: RS256 + a kid.
        const auto header_json = base64url_decode((*parts)[0]);
        if (!header_json) co_return std::nullopt;
        const nlohmann::json header = nlohmann::json::parse(*header_json, nullptr, false);
        if (header.is_discarded() || header.value("alg", "") != "RS256") co_return std::nullopt;
        const std::string kid = header.value("kid", "");

        // Payload: iss must match, exp/nbf must hold.
        const auto payload_json = base64url_decode((*parts)[1]);
        if (!payload_json) co_return std::nullopt;
        const nlohmann::json payload = nlohmann::json::parse(*payload_json, nullptr, false);
        if (payload.is_discarded()) co_return std::nullopt;

        std::string token_iss = payload.value("iss", "");
        std::string want_iss(issuer);
        auto strip_slash = [](std::string& s) { if (!s.empty() && s.back() == '/') s.pop_back(); };
        strip_slash(token_iss);
        strip_slash(want_iss);
        if (token_iss != want_iss) co_return std::nullopt;

        const long now = now_unix != 0
                             ? now_unix
                             : static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                     std::chrono::system_clock::now().time_since_epoch())
                                                     .count());
        if (payload.contains("exp") && payload["exp"].is_number()) {
            if (now >= payload["exp"].get<long>()) co_return std::nullopt;  // expired
        }
        if (payload.contains("nbf") && payload["nbf"].is_number()) {
            if (now < payload["nbf"].get<long>()) co_return std::nullopt;  // not yet valid
        }

        // Signature over "header.payload".
        const auto signature = base64url_decode((*parts)[2]);
        if (!signature) co_return std::nullopt;
        const std::string_view signed_input(token.data(), (*parts)[1].data() + (*parts)[1].size() - token.data());

        const auto keys = co_await keys_for(state, want_iss, std::move(transport));
        if (keys.empty()) co_return std::nullopt;
        const JwkKey* match = nullptr;
        for (const auto& key : keys) {
            if (kid.empty() || key.kid == kid) { match = &key; if (!kid.empty()) break; }
        }
        if (!match) co_return std::nullopt;
        if (!rs256_verify(*match, signed_input, *signature)) co_return std::nullopt;

        // Required claims, checked once the signature holds (#1423): an expiry, and an
        // authorized party that is one of this node's own origins.
        if (!payload.contains("exp") || !payload["exp"].is_number_integer()) co_return std::nullopt;
        if (!payload.contains("azp") || !payload["azp"].is_string() ||
            !clerk_party_allowed(payload["azp"].get<std::string>())) {
            co_return std::nullopt;
        }

        ClerkClaims claims;
        claims.subject = payload.value("sub", "");
        claims.email = payload.value("email", "");
        if (claims.subject.empty()) co_return std::nullopt;  // no subject → no principal to map
        co_return claims;
    } catch (...) {
        co_return std::nullopt;
    }
}

namespace {
ClerkJwksCache& shared_cache() {
    static ClerkJwksCache cache;
    return cache;
}

struct BlockingExchange {
    std::optional<connect::HttpResponse> response;
    runtime::async_event finished;
};

runtime::task<> run_blocking_transport(connect::HttpTransport transport,
                                       connect::HttpRequest request,
                                       std::shared_ptr<BlockingExchange> exchange) {
    try {
        exchange->response = transport(request);
    } catch (...) {
        exchange->response = connect::HttpResponse{0, {}, "JWKS transport failed"};
    }
    exchange->finished.set();
    co_return;
}

runtime::io_task<connect::HttpResponse> offloaded_transport(
    connect::HttpTransport transport, connect::HttpRequest request) {
    auto exchange = std::make_shared<BlockingExchange>();
    runtime::spawn(run_blocking_transport(std::move(transport), std::move(request),
                                          exchange), {});
    const auto [error] = co_await exchange->finished.wait();
    if (error) co_return {error, connect::HttpResponse{}};
    co_return {{}, std::move(*exchange->response)};
}
}  // namespace

runtime::task<std::optional<ClerkClaims>> verify_clerk_jwt_async(
    std::string issuer, std::string token, connect::AsyncHttpTransport transport,
    long now_unix) {
    return shared_cache().verify(std::move(issuer), std::move(token),
                                 std::move(transport), now_unix);
}

std::optional<ClerkClaims> verify_clerk_jwt(
    std::string_view issuer, std::string_view token,
    const connect::HttpTransport& transport, long now_unix) {
    connect::AsyncHttpTransport asynchronous;
    if (transport) {
        asynchronous = [transport](const connect::HttpRequest& request) {
            return offloaded_transport(transport, request);
        };
    }
    return runtime::block_on(verify_clerk_jwt_async(
        std::string(issuer), std::string(token), std::move(asynchronous), now_unix));
}

void clear_clerk_jwks_cache() { shared_cache().clear(); }

}  // namespace zetta::api
