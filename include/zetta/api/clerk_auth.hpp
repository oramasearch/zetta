#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "zetta/connect/http_transport.hpp"

// Clerk JWT verification (#889): the node accepts a Clerk-issued session JWT as a
// bearer, alongside the native token store / zetta_session cookie. Clerk is the
// PREFERRED auth for hosted Studio; the native bearer is the on-prem fallback.
//
// A node with NO issuer configured never calls this — it is bearer-only, so
// air-gapped installs (no egress to Clerk's JWKS) work with zero Clerk
// dependency. Verification uses only the public JWKS (no Clerk secret on the
// node), RS256 via the OpenSSL the node already links.
namespace zetta::api {

// The claims a verified Clerk session JWT carries that the node maps to a
// principal. `subject` is the Clerk user id (the `sub` claim), stable per user.
struct ClerkClaims {
    std::string subject;  // sub — the Clerk user id
    std::string email;    // best-effort, from `email` when present
};

// Verify `token` as a Clerk session JWT issued by `issuer` (e.g.
// https://<instance>.clerk.accounts.dev). Fetches + caches the issuer's JWKS
// (`<issuer>/.well-known/jwks.json`) over `transport` (defaults to the installed
// default HTTP transport), checks the RS256 signature over header.payload
// against the key named by the token's `kid`, and enforces `iss` == issuer and
// `exp`/`nbf` against `now_unix` (0 = read the wall clock). Returns the claims on
// success, nullopt on any failure (bad shape, unknown kid, bad signature, wrong
// issuer, expired). Never throws — the auth gate stays non-throwing.
//
// `transport` is injectable so tests can serve a synthetic JWKS; production
// passes an empty function to use the node's native HTTPS client.
std::optional<ClerkClaims> verify_clerk_jwt(
    std::string_view issuer, std::string_view token,
    const zetta::connect::HttpTransport& transport = {}, long now_unix = 0);

// Does `token` look like a JWT (three base64url segments separated by dots)?
// The gate uses this to route only JWT-shaped bearers to Clerk verification, so
// a native opaque token never triggers a JWKS fetch.
bool looks_like_jwt(std::string_view token);

// Clear the process-wide JWKS cache (tests; also a manual rotation hook).
void clear_clerk_jwks_cache();

}  // namespace zetta::api
