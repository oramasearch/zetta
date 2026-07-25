// Clerk JWT verification (#889). Generates an RSA key in-test, signs an RS256
// JWT, serves the matching JWKS through an injected transport, and asserts the
// full verify path — plus the failure closes (wrong issuer, expired, bad
// signature, wrong key, non-JWT).
#include <gtest/gtest.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <chrono>
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

std::string make_jwt(EVP_PKEY* pkey, const std::string& kid, const std::string& iss,
                     const std::string& sub, long exp) {
    const std::string header = base64url(nlohmann::json{{"alg", "RS256"}, {"typ", "JWT"}, {"kid", kid}}.dump());
    const std::string payload = base64url(nlohmann::json{{"iss", iss}, {"sub", sub}, {"exp", exp}, {"email", "u@x.com"}}.dump());
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

}  // namespace
