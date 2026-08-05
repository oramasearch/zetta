/*
 * Clerk JWT verification (#889). RS256 over the issuer's JWKS, via OpenSSL. See
 * clerk_auth.hpp for the contract. Non-throwing: every failure path returns
 * nullopt, so the auth gate never unwinds.
 */

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
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::api {

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

// The JWKS cache: issuer → keys, with a fetch time. TTL keeps a rotated key set
// from being stale forever while avoiding a fetch per request.
struct CachedJwks {
    std::vector<JwkKey> keys;
    std::chrono::steady_clock::time_point fetched;
};
std::mutex g_jwks_mutex;
std::map<std::string, CachedJwks, std::less<>> g_jwks_cache;
constexpr std::chrono::minutes kJwksTtl{60};

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

// Fetch (or return cached) JWKS for an issuer over the transport.
std::optional<std::vector<JwkKey>> jwks_for(std::string_view issuer,
                                            const zetta::connect::HttpTransport& transport) {
    {
        std::lock_guard<std::mutex> lock(g_jwks_mutex);
        if (auto it = g_jwks_cache.find(issuer);
            it != g_jwks_cache.end() &&
            std::chrono::steady_clock::now() - it->second.fetched < kJwksTtl) {
            return it->second.keys;
        }
    }
    const zetta::connect::HttpTransport& send =
        transport ? transport : zetta::connect::default_http_transport();
    if (!send) return std::nullopt;  // no transport installed → cannot verify

    std::string url(issuer);
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/.well-known/jwks.json";
    zetta::connect::HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.timeout_seconds = 10;
    const zetta::connect::HttpResponse response = send(request);
    if (!response.ok()) return std::nullopt;

    std::vector<JwkKey> keys = parse_jwks(response.body);
    if (keys.empty()) return std::nullopt;
    {
        std::lock_guard<std::mutex> lock(g_jwks_mutex);
        g_jwks_cache[std::string(issuer)] = {keys, std::chrono::steady_clock::now()};
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

void clear_clerk_jwks_cache() {
    std::lock_guard<std::mutex> lock(g_jwks_mutex);
    g_jwks_cache.clear();
}

std::optional<ClerkClaims> verify_clerk_jwt(std::string_view issuer, std::string_view token,
                                            const zetta::connect::HttpTransport& transport,
                                            long now_unix) {
    if (issuer.empty()) return std::nullopt;
    const auto parts = split_jwt(token);
    if (!parts) return std::nullopt;

    // Header: RS256 + a kid.
    const auto header_json = base64url_decode((*parts)[0]);
    if (!header_json) return std::nullopt;
    const nlohmann::json header = nlohmann::json::parse(*header_json, nullptr, false);
    if (header.is_discarded() || header.value("alg", "") != "RS256") return std::nullopt;
    const std::string kid = header.value("kid", "");

    // Payload: iss must match, exp/nbf must hold.
    const auto payload_json = base64url_decode((*parts)[1]);
    if (!payload_json) return std::nullopt;
    const nlohmann::json payload = nlohmann::json::parse(*payload_json, nullptr, false);
    if (payload.is_discarded()) return std::nullopt;

    std::string token_iss = payload.value("iss", "");
    std::string want_iss(issuer);
    auto strip_slash = [](std::string& s) { if (!s.empty() && s.back() == '/') s.pop_back(); };
    strip_slash(token_iss);
    strip_slash(want_iss);
    if (token_iss != want_iss) return std::nullopt;

    const long now = now_unix != 0
                         ? now_unix
                         : static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count());
    if (payload.contains("exp") && payload["exp"].is_number()) {
        if (now >= payload["exp"].get<long>()) return std::nullopt;  // expired
    }
    if (payload.contains("nbf") && payload["nbf"].is_number()) {
        if (now < payload["nbf"].get<long>()) return std::nullopt;  // not yet valid
    }

    // Signature over "header.payload".
    const auto signature = base64url_decode((*parts)[2]);
    if (!signature) return std::nullopt;
    const std::string_view signed_input(token.data(), (*parts)[1].data() + (*parts)[1].size() - token.data());

    const auto keys = jwks_for(want_iss, transport);
    if (!keys) return std::nullopt;
    const JwkKey* match = nullptr;
    for (const auto& key : *keys) {
        if (kid.empty() || key.kid == kid) { match = &key; if (!kid.empty()) break; }
    }
    if (!match) return std::nullopt;
    if (!rs256_verify(*match, signed_input, *signature)) return std::nullopt;

    ClerkClaims claims;
    claims.subject = payload.value("sub", "");
    claims.email = payload.value("email", "");
    if (claims.subject.empty()) return std::nullopt;  // no subject → no principal to map
    return claims;
}

}  // namespace zetta::api
