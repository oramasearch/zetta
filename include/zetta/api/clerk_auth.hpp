#pragma once

#include <chrono>
#include <functional>
#include <memory>
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

// Node-local verifier cache (#1416). One asynchronous fetch per issuer, including
// failures; fresh keys live for 55 minutes, with at most 5 more minutes during
// refresh failures. Failures retry after 5 seconds. Fetches/waits are bounded to
// 2 seconds. The existing 60-minute hard key lifetime is never extended.
// An unknown kid never bypasses those limits or extends key lifetime.
// The clock is injectable for deterministic rotation and outage tests.
class ClerkJwksCache {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    explicit ClerkJwksCache(Clock clock = std::chrono::steady_clock::now);

    // Inputs and transport are owned by the coroutine. Empty transport selects
    // the installed native async client (blocking-only installs are offloaded).
    // All malformed input/fetch errors deny.
    runtime::task<std::optional<ClerkClaims>> verify(
        std::string issuer, std::string token,
        connect::AsyncHttpTransport transport = {}, long now_unix = 0) const;
    void clear();

private:
    struct State;
    std::shared_ptr<State> state_;
};

// Shared process cache used by the HTTP gate for both bearer and cookie JWTs.
// Suspends during a cold fetch without occupying the HTTP executor's thread.
runtime::task<std::optional<ClerkClaims>> verify_clerk_jwt_async(
    std::string issuer, std::string token,
    connect::AsyncHttpTransport transport = {}, long now_unix = 0);

// Synchronous compatibility entry point for non-coroutine callers. Uses the same
// cache and verifier as the HTTP gate; an injected blocking transport is offloaded.
// Checks RS256, issuer, exp/nbf and maps sub/email; nullopt on any failure.
std::optional<ClerkClaims> verify_clerk_jwt(
    std::string_view issuer, std::string_view token,
    const connect::HttpTransport& transport = {}, long now_unix = 0);

// Does `token` look like a JWT (three base64url segments separated by dots)?
// The gate uses this to route only JWT-shaped bearers to Clerk verification, so
// a native opaque token never triggers a JWKS fetch.
bool looks_like_jwt(std::string_view token);

// Clear the process-wide JWKS cache (tests; also a manual rotation hook).
void clear_clerk_jwks_cache();

// The origins a Clerk token may be issued to (its `azp`), process-wide (#1423): the node's
// own Studio origins, from ZETTA_CLERK_AUTHORIZED_PARTIES. A verified token must also carry `exp`.
// Empty, every Clerk token is refused: another product sharing the Clerk instance could otherwise
// mint one this node would accept.
void set_clerk_authorized_parties(std::vector<std::string> parties);

}  // namespace zetta::api
