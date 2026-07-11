# Access control (key-scoped, V1)

zetta's V1 access model is **key-scoped**: a request authenticates with an
**access key**, and thereby inherits exactly the access that key permits along
**two orthogonal axes** —

- its **scope**: *which resources* it may touch (databases, projects), and
- its **capabilities**: *which rights* it may exercise (API / data /
  orchestration actions).

Both are **deny-by-default**: in-scope resources are allowed and everything else
denied; an operation is allowed only with a matching capability and denied
otherwise. This realizes the business rule **RULE-3** / permission rule
**PERM-1** and the decision **ADR-6** (*operate over exactly what the login key
permits; team/partner/per-dataset tiers later*), and implements the closed domain
entities **ENT-20 (Principal)**, **ENT-21 (AccessKey)**, and **ENT-22
(Capability)**. It is part of **FEAT-9 (Governed self-service)**.

> **V1 scope, deliberately.** There is **no bespoke permission model** yet — a
> key's scope + capabilities *are* the access. Finer tiers (team / partner /
> per-dataset, and per-table / per-row granularity) are explicitly deferred; the
> audit trail and approval gates are separate FEAT-9 slices. This slice is the
> types + the enforcement decision, wired into the node and replicated.

## The shape

```
Principal  (ENT-20)   an authenticated actor — user | service | node
  id, kind, name, created

AccessKey  (ENT-21)   a login credential owned by a principal
  id, principal, scope, capabilities, created   (immutable once created)

Scope                 which resources a key permits — deny-by-default
  unrestricted  bool                     the "*" grant (an admin key)
  grants        [ Resource ]             the explicit in-scope resources

Resource              a node-owned resource a grant names / a request targets
  kind  database | project               (coarse, top-level — finer tiers later)
  name  the resource's handle

CapabilitySet         which rights a key may exercise — deny-by-default (no "*")
  grants        [ Capability ]           the explicit granted rights

Capability  (ENT-22)  a deny-by-default grant of a specific right, optionally TTL'd
  domain      api | data | orchestration the governance domain of the right
  action      the specific right         (e.g. query / read / deploy / start_run)
  expires_at  iso8601?                   the TTL; omitted means it never expires

Operation             a right a request wants to exercise / a capability is matched on
  domain  api | data | orchestration
  action  the specific right
```

A **Principal** is an authenticated actor — the subject of access grants and
(in a later slice) the audit trail. A principal owns its **access keys** the same
way a `Project` owns its inputs or a `Database` owns its tables. Access itself is
decided by the *key's scope + capabilities*, not by the principal's kind.

An **AccessKey** is the login credential whose scope + capabilities bound a
principal's reach. A key is an **immutable credential**: it is created once with a
fixed scope + capability set and never edited — to change access, issue a new key
and drop the old one. (A capability's **TTL** still lets a granted right lapse
over time without mutating the key.)

A **Capability** (ENT-22) is the *right/verb* axis, orthogonal to the scope's
*resource* axis: it grants a single, **specific** API / data / orchestration
action, optionally bounded by a TTL. There is **no wildcard/unrestricted
capability** (unlike a scope) — capabilities name specific rights by design.

## The decision (RULE-3)

Enforcement is **deny-by-default**. A `Scope` permits a `Resource` iff the scope
is `unrestricted`, or the resource exactly matches one of its `grants` (same
kind *and* name — there is no cross-kind or wildcard match within a kind in V1).

```cpp
Scope scope;
scope.grant(zetta::governance::ResourceKind::Database, "churn");   // a key scoped to one db

scope.permits({zetta::governance::ResourceKind::Database, "churn"});  // true  — in scope
scope.permits({zetta::governance::ResourceKind::Database, "fraud"});  // false — out of scope
scope.permits({zetta::governance::ResourceKind::Project,  "churn"});  // false — wrong kind
```

A request inherits its key's scope. The node-level entry point is
`Node::authorize`, which resolves the login key by id and returns an
`AccessDecision` (allow/deny + a human-readable reason fit for the future audit
trail). An **unknown or empty key id is denied** (no credential ⇒ no access):

```cpp
auto analyst = node.create_principal("analyst");                 // a user
auto key     = analyst->create_key(scope);                       // scoped key

node.authorize(key->id(), {ResourceKind::Database, "churn"});    // allowed
node.authorize(key->id(), {ResourceKind::Database, "fraud"});    // denied
node.authorize("no-such-key", {ResourceKind::Database, "churn"});// denied
```

`zetta::governance::authorize(const AccessKey&, const Resource&)` is the pure decision over a
known key; `Node::authorize(key_id, resource)` is the seam a request path calls.

## The capability decision (ENT-22)

Capabilities answer a different question than scope: not *which resource* but
*which right*. An operation — a `(domain, action)` pair — is allowed **only** when
the key holds a **matching, unexpired** capability, and denied otherwise
(deny-by-default; there is no wildcard capability). A capability matches an
operation iff its `domain` and `action` are exactly equal, and its TTL (if any)
has not elapsed as of the time of the check.

```cpp
zetta::governance::CapabilitySet caps;
caps.grant(zetta::governance::CapabilityDomain::Data, "read");                  // an open-ended right
caps.grant(zetta::governance::CapabilityDomain::Api, "deploy", "2026-07-01T00:00:00.000Z");  // TTL'd

caps.permits({CapabilityDomain::Data, "read"},  now);   // true  — matching right
caps.permits({CapabilityDomain::Data, "write"}, now);   // false — never granted
caps.permits({CapabilityDomain::Api,  "deploy"}, now);  // true iff now < the TTL
```

The two axes **compose** but stay independent: a full operation-on-a-resource
check asks both `authorize(key, resource)` (scope) and `authorize(key, operation,
now)` (capability), and proceeds only if both allow. Through the node:

```cpp
auto pipeline = node.create_principal("pipeline", PrincipalKind::Service);
zetta::governance::CapabilitySet caps;
caps.grant(CapabilityDomain::Orchestration, "start_run");
auto key = pipeline->create_key(scope, caps);            // scope + capabilities

node.authorize(key->id(), Operation{CapabilityDomain::Orchestration, "start_run"});  // allowed
node.authorize(key->id(), Operation{CapabilityDomain::Data, "write"});               // denied
```

`zetta::governance::authorize(const AccessKey&, const Operation&, std::string_view now)` is the
pure decision (the injectable `now` makes TTL deterministic in tests). Its deny
reason **distinguishes** a right that was never granted from one whose TTL has
expired — context the audit trail will record.
`Node::authorize(key_id, operation)` is the request-path seam; it resolves the key
(denying an unknown/empty id) and supplies the current time.

> **TTL'd; audited.** A capability is the unit of authority for orchestration +
> the sandbox: it is **TTL'd** (an optional expiry, honored by the decision) and,
> in a later FEAT-9 slice, **audited** (`CapabilityGranted`/grant-capability are
> separate command/event slices — see ENT-22's traceability). Granting a
> capability today means issuing a key that carries it.

## Replication + persistence

Principals and keys are **replicated state**, like everything else in zetta (the
Automerge CRDT document is the source of truth; the on-disk tree and in-memory
node are its projection). They use flat catalog keys (`src/core/catalog.hpp`):

```
principal:<uuid>  ->  { kind, name, created }
key:<uuid>        ->  { principal, scope: { unrestricted, grants:[...] },
                        capabilities: [ { domain, action, expires_at? }... ], created }
```

`Node` captures local create/drop/rename into the document, reconciles peer
changes back onto disk on each `tick()`, and persists the document — the same
capture/reconcile path the databases and projects use. The preflight integrity
check flags a **dangling access key** (a `key:` whose `principal` is gone).

On disk, a principal is a UUID-named folder under the data dir (alongside
databases and projects), and its keys nest under it:

```
data_dir/<principal-uuid>/
  principal.json            { id, kind, name, created }
  keys/
    <key-uuid>/
      key.json              { id, principal, scope, capabilities, created }
```

The folder name *is* the id and never changes; rename rewrites `principal.json`.
A key carries no body — its scope + capabilities are the whole record — so unlike
a project input there is nothing to transfer peer-to-peer; the `key:` value
replicates in full.

## JSON

`Principal` metadata and `AccessKey` metadata (with its embedded `Scope` and
`CapabilitySet`) round-trip through JSON via the nlohmann ADL `to_json` hooks, and
are parsed at the trust boundary with `scope_from_json()` /
`capability_set_from_json()` (and the shape checks in `Principal::load()` /
`Node::load_principals()`), which **reject anything malformed** — not an object,
a missing or wrong-typed field, an unknown resource kind / capability domain, a
non-array `grants` / `capabilities`. This matters most for the **scope** and the
**capabilities**: a malformed grant must fail fast rather than silently widen or
narrow access, so a corrupt `key.json` is skipped at load instead of granting the
wrong reach. The `capabilities` field is **optional** (an older key without it
simply grants no rights), but a present-yet-malformed set rejects the whole key.

The contracts are captured as JSON Schemas in
[`schemas/principal.schema.json`](../../schemas/principal.schema.json) and
[`schemas/access_key.schema.json`](../../schemas/access_key.schema.json) (the latter
defines the `scope` + `resource` sub-shapes). As elsewhere in zetta, the
hand-written boundary checks mirror the schemas rather than pulling in a
schema-validator dependency — **keep the schema, the structs, and the parsers in
sync.**

```json
{
  "id": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
  "principal": "a1b2c3d4-...",
  "scope": {
    "unrestricted": false,
    "grants": [
      { "kind": "database", "name": "churn" },
      { "kind": "project",  "name": "churn" }
    ]
  },
  "capabilities": [
    { "domain": "data",          "action": "read" },
    { "domain": "orchestration", "action": "start_run",
      "expires_at": "2026-07-01T00:00:00.000Z" }
  ],
  "created": "2026-06-25T14:03:21.123Z"
}
```

## Where it fits

This is the foundation the rest of FEAT-9 builds on. The **audit trail** (every
graph change / deploy / data access) is now built — each `Node::authorize`
decision records an immutable `data_access` [`AuditEvent`](audit-trail.md). A
later slice threads the login key through the request path (the query executor,
the cluster protocol) so every data access is checked against `Node::authorize` —
both the resource scope and the capability for the operation being performed — and
thereby audited, not just the seam itself; **approval gates** for risky actions
are their own slice, as is **granting a capability** as a first-class
command/event (ENT-22 → `GrantCapability` / `CapabilityGranted`). The finer
access **tiers** (team / partner / per-dataset) extend `ResourceKind` and
`Scope`, and new rights extend `CapabilityDomain` / the open action taxonomy,
without changing this seam. Principals + keys + their scope/capability grants are
now surfaced read-only in the headed [Admin & Settings panel](../gui/admin-panel.md)
(SCR-7 / CMP-5); issuing keys and granting capabilities from the GUI is a later
slice.

## Users, roles, and tokens (the credential store — FEAT-9 / #744)

The V1 model above (a bare **AccessKey** carrying scope + capabilities directly)
is the enforcement engine. Layered **on top of it**, not replacing it, is the
**credential store** the rest of the identity work builds on (epic #743): a
**user + role + token** model, all replicated through the same
capture/reconcile/persist path as principals.

```
User   (user:<uuid>)         an authenticatable identity — a Principal of kind
  name, status, created        User in the new model. Its rights come from the
                               roles assigned to it; a bearer authenticates AS a
                               user by presenting one of its tokens.

Role   (role:<uuid>)         a named permission set: a Scope (resources) + a
  name, scope, capabilities,   CapabilitySet (rights) — the SAME two axes an
  created                      AccessKey carries. Built-in admin/writer/reader
                               ship now; custom/editable roles are #746.

UserRole (user_role:<uuid>)  a user↔role assignment. A user's effective
  user, role                   permissions are the union of its roles.

Token  (token:<uuid>)        a bearer credential that authenticates as a user,
  user, type (api|access),     stored HASHED at rest. The plaintext secret is
  hash, salt, created,         shown ONCE at creation and never persisted; only
  expires_at?, revoked         hash = SHA-256(salt || secret) + salt is stored.
```

### Hashed tokens

A token's secret is a 256-bit random value from the OS CSPRNG (`RAND_bytes`).
At creation the **plaintext is returned exactly once**; at rest only its salted
hash lives in the CRDT + `token.json`. Validation (`Node::authenticate_token`)
re-hashes a presented bearer and **constant-time compares** it against every
live token (`CRYPTO_memcmp`), rejecting revoked, expired, or disabled-user
tokens, and resolves to the owning **User**. The crypto lives in one OpenSSL TU,
`src/core/governance/token_secret.cpp` — the second (after
`src/core/cluster/security.cpp`) allowed to name libcrypto directly. The
self-contained `zetta::util::sha256_hex` is deliberately **not** used for secrets
(it is not constant-time — "content fingerprints, never secrets").

### Built-in roles

`builtin_role_permissions(name)` seeds three roles, all unrestricted in scope
(they reach every resource) and differing only in rights:

| role   | rights |
|--------|--------|
| admin  | api:query, api:deploy, api:manage_users, data:read, data:write, orchestration:start_run |
| writer | api:query, data:read, data:write, orchestration:start_run |
| reader | api:query, data:read |

Deny-by-default still holds: a right a role doesn't list is denied.

### Bootstrap owner (closes #733)

On node start, `Node::bootstrap_owner(secret)` runs only when the **user store is
empty** and a secret is available from `--admin-token` / `ZETTA_ADMIN_TOKEN`. It
mints an `owner` user, ensures the built-in roles exist, assigns `owner` the
`admin` role, and issues an **access** token whose secret is the operator's — so
a fresh node is never locked out of its own governance. The node logs **where**
the token record lives, **never** the secret value (the operator already holds
it). With no secret supplied, bootstrap is a no-op and the node prints how to
seed one.

### Read views + issuance

`system.users`, `system.roles`, and `system.tokens` are read views
(`src/core/query/executor.cpp`) mirroring `system.principals`/`system.keys`. The
**tokens view exposes no secret material** — id, user, type, created, expiry, and
revoked only, never the hash or salt. A minimal SQL issuance surface exists:
`CREATE USER <name>` and `CREATE TOKEN FOR <user> [TYPE access|api]` (the latter
returns the plaintext secret once, in a single result row). Tokens can also be
issued programmatically via `Node::create_token`.

> **Replication + persistence.** Users, roles, tokens, and assignments replicate
> exactly like principals: local create/drop/revoke are captured into the
> Automerge document (`user:` / `role:` / `token:` / `user_role:` keys), each
> `Node::tick()` reconciles the on-disk projection (materialize-only), and each
> entity persists to a UUID-named folder under the data dir
> (`user.json` / `role.json` / `token.json` / `user_role.json`). A token's hash
> replicates in full; the plaintext never enters the document.

> **What's next.** The *store* above is the foundation; the **default-deny gate**
> below (`--require-auth`, #745) consumes it. Still ahead: full role management is
> #746; scoped grants down to table/column/blob/MCP tool are #4; row-level
> security is #5.

## The default-deny gate (`--require-auth` — #745)

Every control surface — `/mcp`, `/v1/sql`, all `/v1/*` (REST + specs +
openapi + llms.txt), and the governed serving endpoints — is fronted by **one
auth gate**: a single middleware at the router seam (`src/core/api/http_server.cpp`,
beside `router.use(cors())`), not a shared-secret check duplicated per handler.
It resolves the acting identity **once** per request and hands it to the matched
handler through the request's `route_data`; the handlers carry no auth logic of
their own. The gate has three modes:

- **`--require-auth` (secure-by-default).** Every path is closed unless the
  request carries `Authorization: Bearer <secret>` whose secret resolves — via
  the token store (`Node::authenticate_token`: salted-hash + **constant-time**
  compare, rejecting revoked/expired tokens and disabled users) — to an **active
  user**. Anything else (no header, wrong secret, revoked/expired token, disabled
  user) is **401**. There is **no anonymous path**, and the legacy `--mcp-token`
  shared secret is **not** honored in this mode. The resolved **user** becomes the
  acting identity, and the capability check runs against that user's **effective
  permissions**.
- **Legacy `--mcp-token` (back-compat).** With no `--require-auth`, a configured
  shared secret still gates at the door by exact match, and the token doubles as
  the acting AccessKey id — the pre-#745 behavior, unchanged.
- **Unauthenticated-local (the loopback default).** With neither flag, there is
  no gate: the acting key is whatever bearer was presented (empty = the audited
  local session). This keeps the **loopback GUI** working as a trusted local
  session with no credential — the deliberate, documented exception, only sane on
  a loopback bind. `--require-auth` closes even this path.

The CORS preflight (`OPTIONS`) is answered by the `cors()` middleware **before**
the gate, so a credential-less browser preflight is never blocked.

### From bearer to a capability decision

Under the gate, the acting identity is a **user id**, so the operation is checked
against the user's roles, not against an AccessKey:

```cpp
node.authenticate_token(bearer)   // -> the owning User, or none (deny-by-default)
node.authorize_user(user_id, op)  // allowed iff some assigned role's CapabilitySet
                                  //   permits `op` now (union of the user's roles)
```

`Node::authorize_user` is the token-resolved counterpart to the AccessKey
`Node::authorize(key_id, op)`: an unknown or **disabled** user is denied, and an
operation is allowed only when one of the user's assigned roles (via `UserRole`)
holds a matching, unexpired capability — the same deny-by-default, TTL-honoring
decision, over a user's *effective* reach. `registry.cpp` (MCP + REST) and
`serving.cpp` (predict/search) branch on this: a token-resolved user checks
`authorize_user`; a legacy AccessKey id checks `authorize`; and an empty acting
key is the unauthenticated-local session — reachable **only** with the gate off.
Bootstrap seeds `owner` with the `admin` role, so the `--admin-token` secret both
opens the gate and (as admin) satisfies the capability check.

> **Scope note.** #745 centralizes enforcement + makes it mandatory; it does not
> expand the *model*. The built-in roles are still the minimal admin/writer/reader
> sets (#746 adds custom roles), and scope stays coarse (#4 adds table/column/blob
> grants). An admin token therefore reaches every route but is still deny-by-default
> on rights its role does not name.

## Pointers

- `include/zetta/governance/user.hpp` — `zetta::governance::User` + `UserStatus` (an authenticatable identity; #744).
- `include/zetta/governance/role.hpp` — `zetta::governance::Role` / `RolePermissions` / `builtin_role_permissions` (named permission sets).
- `include/zetta/governance/token.hpp` — `zetta::governance::Token` / `TokenValue` / `TokenType` (hashed bearer credentials).
- `include/zetta/governance/user_role.hpp` — `zetta::governance::UserRole` (the user↔role assignment).
- `include/zetta/governance/token_secret.hpp` — `generate_token_secret` / `hash_token_secret` / `constant_time_equal` (the OpenSSL token crypto).
- `include/zetta/node.hpp` — user/role/token CRUD + `Node::bootstrap_owner` / `Node::authenticate_token` / `Node::authorize_user` (the token-resolved capability decision, #745).
- `include/zetta/api/server.hpp` — `ServerOptions::require_auth` (the default-deny gate flag, #745).
- `src/core/api/http_server.cpp` — the ONE auth-gate middleware at the router seam (`bearer → user`, else 401; #745).
- `tests/api/test_auth_gate.cpp` — the gate + `authorize_user` coverage (401 on every path without a token; the owner token opens them; per-user capability verdicts; back-compat with the gate off).
- `include/zetta/governance/principal.hpp` — `zetta::governance::Principal` (ENT-20) + `PrincipalKind`.
- `include/zetta/governance/access_key.hpp` — `zetta::governance::AccessKey` (ENT-21), `AccessDecision`,
  `authorize()`.
- `include/zetta/governance/scope.hpp` — `zetta::governance::Scope` / `Resource` / `ResourceKind` (the
  deny-by-default *resource* decision).
- `include/zetta/governance/capability.hpp` — `zetta::governance::Capability` / `CapabilitySet` /
  `Operation` / `CapabilityDomain` (ENT-22; the deny-by-default *rights* decision,
  TTL'd).
- `include/zetta/node.hpp` — principal CRUD + `Node::authorize` (resource +
  operation overloads) / `find_access_key`.
- [admin-panel.md](../gui/admin-panel.md) — the headed **Admin & Settings panel** that
  renders principals/keys + scope/capability grants (and the audit log) read-only.
- [replication.md](../cluster/replication.md) — the CRDT model these entities replicate through.
- [projects.md](../pipeline/projects.md) — the sibling top-level entity whose
  capture/reconcile path this mirrors.
