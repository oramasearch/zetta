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
known key; `Node::authorize(key_id, resource)` is the boundary a request path calls.

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
`Node::authorize(key_id, operation)` is the request-path boundary; it resolves the key
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
thereby audited, not just the boundary itself; **approval gates** for risky actions
are their own slice, as is **granting a capability** as a first-class
command/event (ENT-22 → `GrantCapability` / `CapabilityGranted`). The finer
access **tiers** (team / partner / per-dataset) extend `ResourceKind` (which already
has `Topic` for pub/sub, a name-prefix grant like `Blob`, #1029) and
`Scope`, and new rights extend `CapabilityDomain` / the open action taxonomy,
without changing this interface. Principals + keys + their scope/capability grants are
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
  created                      AccessKey carries. Built-in admin/writer/reader +
                               custom/editable roles (#746, below).

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
(they reach every resource) and differing only in rights. They are defined in
terms of the **permission vocabulary** (below), not raw capabilities:

| role   | permissions |
|--------|-------------|
| admin  | *every* permission — `sql:read/write/ddl`, `mcp:call`, `serving:read`, `users:manage`, `roles:manage`, `tokens:manage`, `cluster:admin`, **plus the orchestration control surface** `pipelines:manage`, `serving:manage`, `cloud:manage` (#799), **and the pub/sub atoms** `pubsub:*` (#1029) |
| writer | `sql:read`, `sql:write`, `sql:ddl`, `mcp:call`, `serving:read` |
| reader | `sql:read`, `serving:read`, `mcp:call` |

`writer` and `reader` are **unchanged** — they do **not** carry the orchestration
atoms; only `admin` does.

Deny-by-default still holds: a right a role doesn't list is denied. The node
**reconciles** these on **every** start (`Node::ensure_builtin_roles`, idempotent
— independent of the owner bootstrap below, which only runs on a fresh store):
they always exist and always mean exactly the sets above. So a **binary upgrade
that adds a capability heals a built-in role frozen at an older vocabulary** —
otherwise an upgraded cluster's `admin` (seeded once into the replicated store)
keeps denying the new right. Every role carrying a built-in name is repaired in
place (CRDT multi-leader seeding can leave duplicates; all converge to canonical).

### Bootstrap owner (closes #733)

On node start, bootstrap runs when the **user store is empty**. It mints an
`owner` user, ensures the built-in roles exist, assigns `owner` the `admin` role,
and issues an **access** token — so a fresh node is never locked out of its own
governance. Since auth is now always on (#799), a fresh node with the MCP server
enabled **always** ends up with an owner token, by one of two paths:

- **Secret supplied** (`--admin-token` / `ZETTA_ADMIN_TOKEN`; the operator always
  injects `ZETTA_ADMIN_TOKEN`). `Node::bootstrap_owner(secret)` seeds the owner
  from that secret — hashed at rest, **never** logged (the operator already holds
  it); the node logs only **where** the token record lives.
- **No secret supplied — never-locked-out auto-seed (#799).** On a fresh node with
  the MCP server enabled and an empty user store, the node **generates** an owner
  access token and prints it **once** ("SAVE THIS — shown only once"), so a node
  that would otherwise be unreachable under the always-on gate is never locked
  out. The generated secret is shown that one time and only its hash is stored.

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
> below (#745, always on since #799) consumes it, and **RBAC role management** (the
> permission vocabulary, custom roles, user↔role assignment — #746) is the section
> after it, and **fine-grained scoped grants** (table/column/blob/MCP tool — #4,
> #747) the section after that. Still ahead: row-level security is #5.

## The default-deny gate is always on

> **Shipped (One Orama, #799):** the platform now runs auth **and** inter-node mTLS
> **always on**. The default-deny gate below is **non-optional** — every HTTP/MCP/REST path requires a
> bearer that resolves to a user, else 401 — and there is **no** auth flag or toggle: the `--require-auth`
> flag and the `ZETTA_REQUIRE_AUTH` env were both **removed** (passing `--require-auth` is now an
> unknown-option error). Inter-node peer mTLS is likewise always on; the
> peer `--cluster-plaintext` escape hatch was **removed** (see [../cluster/security.md](../cluster/security.md)).
> The legacy anonymous HTTP path and the `--mcp-token` shared secret are **gone** from the binary — but the
> in-process headed-GUI local session is **unaffected** (it authorizes in-process, not through the HTTP
> gate; see below). The Orama operator injects `ZETTA_ADMIN_TOKEN` to seed the admin owner and renders
> **no** `--cluster-plaintext` ([orama-operator#92](https://github.com/oramasearch/orama-operator/issues/92)).

Every control surface — `/mcp`, `/v1/sql`, all `/v1/*` (REST + specs +
openapi + llms.txt), and the governed serving endpoints — is fronted by **one
auth gate**: a single middleware at the router boundary (`src/core/api/http_server.cpp`,
beside `router.use(cors())`), not a shared-secret check duplicated per handler.
It resolves the acting identity **once** per request and hands it to the matched
handler through the request's `route_data`; the handlers carry no auth logic of
their own. The gate is **always on** (#799) — there is no mode switch:

- **Default-deny, always (#745; mandatory since #799).** Every path is closed
  unless the request carries `Authorization: Bearer <secret>` whose secret
  resolves — via the token store (`Node::authenticate_token`: salted-hash +
  **constant-time** compare, rejecting revoked/expired tokens and disabled
  users) — to an **active user**. Anything else (no header, wrong secret,
  revoked/expired token, disabled user) is **401**. There is **no anonymous HTTP
  path** and **no shared-secret door**: the resolved **user** becomes the acting
  identity, and the capability check runs against that user's **effective
  permissions**.
- **No flag, no toggle.** There is no `--require-auth` flag and no
  `ZETTA_REQUIRE_AUTH` env — both were removed (#799). The gate is unconditional;
  nothing turns it off. (Passing the old `--require-auth` is now an unknown-option
  error, exit 2.)
- **No shared-secret path anywhere.** The legacy `--mcp-token` CLI flag is gone,
  and the embedded `ApiServer` no longer carries a `bearer_token` (or
  `require_auth`) option — its gate is **unconditional**. A bearer that resolves to
  a user via the token store is the *only* way in; there is no shared secret to
  configure. (The OpenAPI document still *names* its security scheme `bearer_token`
  — that is just the scheme identifier saying a bearer is required.)
- **The in-process headed-GUI local session is unaffected.** The governed console
  authorizes **in-process** (`node.cpp`), not through this HTTP gate, so the
  desktop still runs as a trusted local session with no bearer. This is the
  deliberate, in-process exception — it is **not** an HTTP path, and no network
  caller can reach it.

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
`authorize_user`; a legacy AccessKey id (embedder/library only) checks
`authorize`; and an empty acting key is the trusted **in-process** session — now
reachable **only** in-process (the headed-GUI governed console), never over the
always-on HTTP gate. Bootstrap seeds `owner` with the `admin` role, so the
`ZETTA_ADMIN_TOKEN` secret both opens the gate and (as admin) satisfies the
capability check.

> **Scope note.** #745 centralizes enforcement + makes it mandatory; it does not
> expand the *model*. Role management (custom roles, the permission vocabulary) is
> #746, below; fine-grained object scoping (table/column/blob/MCP tool) is #747,
> further below. An admin token therefore reaches every route but is still
> deny-by-default on rights its role does not name.

## RBAC roles: the vocabulary, custom roles, assignment (FEAT-9 / #746)

RBAC turns roles into a first-class, self-service surface on top of the store
(#744) and the gate (#745): a referenceable **permission vocabulary**, **custom
roles** created + edited via SQL, and **user↔role assignment** — all gated on the
`roles:manage` permission, all replicated like every other entity.

### The permission vocabulary

A **permission** is a stable product-level atom — a `<domain>:<action>` name —
that maps onto one or more concrete `Capability{domain,action}` the enforcement
layer decides against. It is the layer *over* the three coarse `CapabilityDomain`
values (`api`/`data`/`orchestration`): the vocabulary names are what a `GRANT`
statement accepts and what `system.roles` renders, while the operations they
expand to are what `Node::authorize_user` / `Node::authorize` check. The canonical
list lives in one place — `include/zetta/governance/permission.hpp`
(`permission_vocabulary()`) — so the grammar, the built-in seed, and the read view
can never disagree:

| permission | maps onto Capability{domain, action} | what it gates |
|------------|--------------------------------------|---------------|
| `sql:read`      | `data:read`                    | read rows via SQL / the query engine |
| `sql:write`     | `data:write`                   | insert/update/delete rows via SQL |
| `sql:ddl`       | `data:ddl`                     | create/drop databases + tables |
| `mcp:call`      | `api:query`                    | invoke the MCP/REST capability surface (the API-call right) |
| `serving:read`  | `data:predict`, `data:search`  | call the governed serving routes (predict + search) |
| `users:manage`  | `api:manage_users`             | create/disable/drop users |
| `roles:manage`  | `api:manage_roles`             | create/edit/drop roles + assign them |
| `tokens:manage` | `api:manage_tokens`            | issue/revoke tokens |
| `cluster:admin` | `api:cluster_admin`            | cluster-wide administration |
| `pubsub:publish` / `pubsub:subscribe` / `pubsub:send_wildcard` | `data:pubsub_publish` / `data:pubsub_subscribe` / `data:pubsub_send_wildcard` | publish to, read from, and address wildcards in pub/sub (#1029, [../pubsub/governance.md](../pubsub/governance.md)) |
| `pubsub:create` / `pubsub:manage` | `orchestration:pubsub_create` / `orchestration:pubsub_manage` | create (incl. auto-create) / drop and reconfigure topics |
| `pipelines:manage` | all pipeline, dataset/connector, graph, agent/profile/plan, and decision-policy orchestration operations in `permission.cpp` | drive the authoring → gate → deploy → run → governed-action control surface over MCP/REST, including `create_policy`, `update_policy`, `decide`, and `approve_decision` (#1104) |
| `serving:manage`   | `orchestration:{expose_model, unexpose_model, expose_index, unexpose_index}` | expose / unexpose models + indexes for governed serving |
| `cloud:manage`     | `orchestration:{cloud_inspect, detect_provisioners, add_nodes, approve_consent, deny_consent}` | inspect targets, detect provisioners, add nodes, decide consent challenges |

The `sql:*` and `serving:*` data rights and the `*:manage` / `cluster:admin`
admin rights are enforced where their surface already checks them; the finer
per-statement SQL enforcement (which `sql:read` vs `sql:write` a given statement
needs, masking, RLS) is the scoped-grants / RLS work (#4 / #5). `mcp:call`
(`api:query`) is what all read capabilities + the `sql` capability check today, so
every built-in carries it.

The three **orchestration atoms** (`pipelines:manage` / `serving:manage` /
`cloud:manage`, #799) cover every registry **Orchestration** op — the pipeline
authoring/deploy/run flow, model+index exposure, and the cloud control surface.
The capability model is exact-match (no wildcard), so each op must be named by some
atom or **no** role could ever grant it. These went unwired while the only way to
reach orchestration over HTTP/MCP was the (now-removed) unauthenticated-local
bypass, which skipped the capability check entirely; with **auth always on** the
bootstrap owner (`admin` = the whole vocabulary) must actually hold them, so it can
drive orchestration over HTTP/MCP/REST. **Only `admin` gets them** — `writer` and
`reader` are unchanged. The admin-covers-every-registry-op invariant is guarded by a
test (`AdminRoleCoversRegistry` in `tests/api/test_mcp_server.cpp`).

### Custom roles + the SQL grammar

Custom roles are created and edited through a minimal SQL grammar, **gated on
`roles:manage`** (checked inside the executor against the acting identity the gate
resolved — an empty acting key is the trusted **in-process** local session and is
allowed):

```sql
CREATE ROLE analyst;                                   -- empty custom role (deny-by-default)
GRANT sql:read, serving:read TO ROLE analyst;          -- add permissions from the vocabulary
REVOKE serving:read FROM ROLE analyst;                 -- narrow it
DROP ROLE analyst;                                     -- remove it (+ its assignments)

GRANT ROLE analyst TO dana;                            -- assign a role to a user
REVOKE ROLE analyst FROM dana;                         -- unassign it
```

`GRANT`/`REVOKE` of a permission list validate every name against the vocabulary
first (an unknown permission fails the whole statement). A custom role starts with
no rights and gains exactly the permissions granted — nothing more. The same
operations exist programmatically on `Node` (`create_role`, `grant_permissions`,
`revoke_permissions`, `drop_role`, `assign_role`, `unassign_role`).

### Effective permissions = the union across roles

A user's effective reach is the **union** of its assigned roles' capability sets —
already the contract of `Node::authorize_user` (#745): the operation is allowed as
soon as *any* assigned role grants a matching, unexpired right, deny-by-default
otherwise. Custom roles + assignments feed the exact same decision as the
built-ins; assigning a role grants its permissions, unassigning revokes them, and
dropping a role removes it from every user (its assignments go with it).

### Read views

`system.roles` gains a **`permissions`** column rendering each role's set back in
the vocabulary (the reverse of the expand — a permission shows iff *all* its
operations are granted), alongside the raw `scope`/`capabilities`. `system.user_roles`
is the new (user, role) assignment view — the many-to-many that feeds each user's
effective permissions. Neither exposes secret material.

## Fine-grained scoped grants (FEAT-9 / #747)

RBAC (#746) grouped rights into roles, but a granted right still reached **every**
object of its kind. Scoped grants add the object tier: a permission can be bound to
a **specific object** — a table, a **column set**, a **blob** (by id or prefix), or
an **MCP tool** — and the enforcement layer narrows access to exactly what the
object covers.

### The object tiers

`ResourceKind` (`include/zetta/governance/scope.hpp`) grows from the two coarse
tiers (`database` / `project`) to include `table` / `column` / `blob` / `tool`. A
`Resource` gains two optional fields the fine tiers use — `database` (the owning db
of a table/column) and `columns` (a column set) — so a coarse resource still
serializes exactly as before (backward compatible on disk + over the CRDT):

```
kind      name                database   columns
database  db name             —          —
table     table name          owning db  —
column    table name          owning db  the column set
blob      blob id / prefix    —          —
tool      MCP tool name       —          —
```

`object_covers(granted, target)` is the deny-by-default covering decision: a
**database** grant covers every table/column in it; a **table** grant covers the
whole table; a **column-set** grant covers the table for reachability and exactly
its columns; a **blob** grant is a **prefix** (covers every id that starts with it);
a **tool** grant is exact.

### The grant model

A role carries flat `capabilities` (the coarse, **unscoped** rights it grants on
every object — what the built-ins use) **plus** a `scoped_grants` list — each a
`{operation, object}` pair (`include/zetta/governance/grant.hpp`). The two compose:

- a **flat** right (`GRANT sql:read TO ROLE r`) reaches every object — unchanged;
- a **scoped** right (`GRANT sql:read ON db.t(a,b) TO ROLE r`) reaches only the
  object it covers.

A user's effective scoped grants are the **union across its roles**. `blob:read`
(`data:blob_read`) is added to the vocabulary as the object-scopable large-object
read right.

### The `ON` grammar

`GRANT`/`REVOKE` take an optional `ON <object>` clause; without it, the grant is
role-wide (flat), exactly as #746:

```sql
GRANT sql:read  ON sales.customers            TO ROLE reader;   -- a whole table
GRANT sql:read  ON sales.customers(id, email) TO ROLE reader;   -- a column set
GRANT sql:read  ON sales.*                    TO ROLE reader;   -- a whole database
GRANT sql:write ON sales.customers(status)    TO ROLE editor;   -- writable columns
GRANT mcp:call  ON TOOL search                TO ROLE reader;   -- one MCP tool
GRANT blob:read ON BLOB 2026/                 TO ROLE archivist;-- a blob prefix
REVOKE sql:read ON sales.customers(email)     FROM ROLE reader;
```

`ON DATABASE <db>` and `ON <db>.*` are equivalent; `TABLE` before `db.table` is
optional. The object is dialect-neutral in the AST (`GrantObject`) and translated to
a `governance::Resource` in the executor. Programmatically:
`Node::grant_scoped_permissions` / `revoke_scoped_permissions`.

### Enforcement (deny-by-default, admin/unrestricted bypasses)

Every check bypasses object scoping for three identities: the trusted
in-process local session (empty acting key), a legacy AccessKey (it carries
only coarse scope/caps), and any user holding the operation as a **flat** right (so
**admin — flat everything — reaches every object**). Only a user whose right is
*exclusively* object-scoped is narrowed.

- **SQL executor** (`src/core/query/executor.cpp`, the one choke point every
  statement flows through): `Node::column_scope(identity, op, db, table)` returns the
  identity's column access for a table. A `SELECT` on a table with **no** covering
  grant is denied; columns the caller lacks `sql:read` on are **masked** to null in
  the output (both `SELECT *` and a projected list), and a `WHERE`/`GROUP BY` over a
  masked column is refused (no inference). An `INSERT` to a table or column with no
  `sql:write` grant is **rejected**.
- **MCP / REST** (`src/core/api/registry.cpp`): the invoke right for the capability
  surface is `mcp:call`, now scopable per tool. `tools/call` (and the REST
  `/v1/<capability>`, the same `Registry::call` choke point) checks
  `Node::may_call_tool` — a deny is a clean `unauthorized` with no side effect. The
  MCP `tools/list` (`src/core/api/http_server.cpp`) is **filtered** to the tools the
  caller may invoke.
- **Blobs** (`src/core/api/serving.cpp`, `serve_blob`): `Node::authorize_blob` gates
  a large-object body read on `blob:read` covering the id (a flat right or a prefix
  grant).

`system.roles` gains a **`scoped_grants`** column rendering each grant as
`<perm> ON <object>` (e.g. `sql:read ON sales.customers(id, email)`).

## Row-level security (FEAT-9 / #748 — closes epic #743)

Scoped grants decide *which columns* of a table an identity may touch; **row-level
security (RLS)** decides *which rows*. It is Postgres-style: a **policy** names a
table + a command class and carries a **`USING`** read filter and/or a **`WITH
CHECK`** write filter — SQL predicates the executor evaluates per row against the
acting identity. This is the last slice of the authz epic.

### `system.policies` (a new `policy:` catalog entity)

A policy is replicated state, projected on disk exactly like a role (`policy:<uuid>`
in the CRDT, a UUID-named folder with `policy.json`, the same capture/reconcile
path). Its value is:

```
policy:<uuid> -> { name, database, table, command, using?, check?, roles?, created }
```

`command` is `select` / `insert` / `update` / `all`; `roles` is the role-name set
the policy applies to (**empty = every role**); `using` / `check` are the stored
predicate expressions (opaque JSON to the governance layer — the query layer owns
their grammar + evaluation). `system.policies` is the read view (name, table,
command, the `using` / `check` predicates rendered back to text, roles). The value
round-trips through `policy_value_from_json` at the trust boundary — a malformed
policy is **skipped**, never silently enforced as open.

### The grammar

```sql
CREATE POLICY own_docs ON sales.orders FOR SELECT
  USING (owner = current_user());                       -- read filter, every role

CREATE POLICY own_writes ON sales.orders FOR INSERT
  WITH CHECK (owner = current_user());                  -- write filter

CREATE POLICY analyst_rows ON sales.orders FOR ALL
  TO ROLE analyst
  USING (owner = current_user())                        -- reads: only own rows
  WITH CHECK (owner = current_user());                  -- writes: only own rows

CREATE POLICY audit_all ON sales.orders FOR SELECT
  USING ('auditor' = ANY(current_roles()));             -- a role sees everything

DROP POLICY own_docs;
```

`FOR SELECT` needs a `USING`; `FOR INSERT`/`UPDATE` needs a `WITH CHECK` (`USING`
is accepted as the check fallback). Both the grammar (`src/core/query/postgres.cpp`,
kept out-of-line + built at `-O1` like the rest of the auth DDL) and the DDL
execution are gated on the **`roles:manage`** permission — policy management is an
admin function, reusing the roles vocabulary rather than adding one.

### Context functions

Two functions resolve against the acting identity, usable anywhere a value is:

- **`current_user()`** → the acting user's **name** (so `owner = current_user()`
  matches rows whose `owner` column equals the caller's username).
- **`current_roles()`** → the caller's **role-name set**, tested with
  `<value> = ANY(current_roles())` (membership) — e.g. `'admin' = ANY(current_roles())`.

Predicates **reuse the existing `WHERE` expression evaluator** (`compare_cell`) — no
second predicate language. A predicate that can't resolve (an absent column, a
corrupt stored predicate) **fails closed** (the row does not match) and never widens
access.

### Enforcement (deny-by-default; admin / unrestricted / local bypass)

The executor is the one choke point — the same per-statement boundary that #747 enforces
column grants on:

- On **`SELECT`**, the applicable policies' `USING` predicates are AND-ed into the
  row scan: the caller sees only rows an applicable policy admits. Multiple
  applicable policies are **permissive** (OR-ed), as in Postgres.
- On **`INSERT`**, each new row must satisfy an applicable `WITH CHECK` — a violating
  row **rejects** the statement.
- **Deny-by-default**: RLS is *engaged* for a table as soon as any policy governs
  that table+command. On an engaged table, a caller with **no applicable policy**
  (its role set doesn't intersect any policy's `roles`) sees **no rows** / can write
  **no rows** — a policy `TO ROLE analyst` narrows analysts to their own rows and
  shuts everyone else out (unless another policy admits them).
- **`GROUP BY`** over a policy-protected table is refused for a non-bypass caller
  (the numeric DDF aggregate path can't apply a row predicate — refuse rather than
  leak an aggregate over hidden rows).

Three identities **bypass** RLS entirely (mirroring the #747 object-scoping bypass):
the trusted in-process local session (empty acting key), a legacy AccessKey,
and any **admin** user (one holding `cluster:admin` — the built-in `admin` does).
So **admin sees every row regardless of policy**. `Node::bypasses_row_security`
is the decision; `current_user()` / `current_roles()` are resolved from the acting
user via `find_user_by_id` + `roles_for_user`.

## Pointers

- `include/zetta/governance/user.hpp` — `zetta::governance::User` + `UserStatus` (an authenticatable identity; #744).
- `include/zetta/governance/role.hpp` — `zetta::governance::Role` / `RolePermissions` / `builtin_role_permissions` (named permission sets).
- `include/zetta/governance/permission.hpp` — the RBAC **permission vocabulary** (#746): `permission_vocabulary()` / `permission_operations()` / `capabilities_for_permissions()` / `permissions_of()` / `builtin_role_permission_names()` (the atoms → `Capability{domain,action}` mapping).
- `include/zetta/node.hpp` — role management (#746): `Node::create_role` / `grant_permissions` / `revoke_permissions` / `drop_role` / `ensure_builtin_roles` (idempotent reconcile) / `assign_role` / `unassign_role`.
- `src/core/query/postgres.cpp` + `src/core/query/executor.cpp` — the SQL grammar (`CREATE/DROP ROLE`, `GRANT/REVOKE … TO/FROM ROLE`, `GRANT/REVOKE ROLE … TO/FROM <user>`) + its `roles:manage` gate and the `system.roles` / `system.user_roles` views (#746).
- `tests/governance/test_roles.cpp` — the RBAC coverage (#746): the vocabulary mapping, built-ins seeded + reconciled, a custom subset grants exactly those, assignment/unassignment changes reach, `roles:manage` required, effective = union across roles, the read views.
- `include/zetta/governance/grant.hpp` — scoped grants (#747): `ScopedGrant` (`{operation, object}`), `object_covers`, `column_access_for`, `object_permitted`, and `ColumnAccess`.
- `include/zetta/node.hpp` — object decisions (#747): `Node::authorize_object` / `column_scope` / `may_call_tool` / `authorize_blob`, and `grant_scoped_permissions` / `revoke_scoped_permissions`.
- `src/core/query/executor.cpp` — the SQL choke point that denies ungranted tables, masks ungranted columns on read, and rejects writes to ungranted columns (#747); `src/core/api/registry.cpp` (per-tool `mcp:call`), `src/core/api/http_server.cpp` (`tools/list` filter), `src/core/api/serving.cpp` (`serve_blob`).
- `tests/governance/test_scoped_grants.cpp` + `tests/api/test_scoped_grants_api.cpp` — the scoped-grants coverage (#747): covering decision, column masking, table denial, write rejection, per-tool `mcp:call` + `tools/list` filter, blob-prefix serve, admin/local bypass.
- `include/zetta/governance/policy.hpp` — row-level security (#748): `zetta::governance::Policy` / `PolicyValue` / `policy_value_from_json` / `policy_schema` (the `policy:<uuid>` entity + its trust boundary).
- `include/zetta/query/ast.hpp` — the RLS predicate AST (#748): `PolicyPredicate` / `PolicyTerm` / `PolicyCommand` + `CreatePolicyStmt` / `DropPolicyStmt` (the dialect-neutral parsed form the executor evaluates with `compare_cell`).
- `include/zetta/node.hpp` — policy CRUD + the RLS bypass decision (#748): `Node::create_policy` / `drop_policy` / `find_policy` / `policies` / `bypasses_row_security`.
- `src/core/query/postgres.cpp` + `src/core/query/executor.cpp` — the `CREATE/DROP POLICY` grammar + its `roles:manage` gate, the `system.policies` view, and the per-statement `USING` (SELECT) / `WITH CHECK` (INSERT) row rewrite reusing the WHERE evaluator (#748).
- `tests/governance/test_row_security.cpp` + `tests/api/test_row_security_api.cpp` — the RLS coverage (#748): `USING (owner = current_user())` limits a non-admin to its own rows, a `WITH CHECK` violation is rejected, admin bypasses, a role-scoped policy applies only to that role (deny-by-default otherwise), `current_roles()` membership, policies replicate.
- `include/zetta/governance/token.hpp` — `zetta::governance::Token` / `TokenValue` / `TokenType` (hashed bearer credentials).
- `include/zetta/governance/user_role.hpp` — `zetta::governance::UserRole` (the user↔role assignment).
- `include/zetta/governance/token_secret.hpp` — `generate_token_secret` / `hash_token_secret` / `constant_time_equal` (the OpenSSL token crypto).
- `include/zetta/node.hpp` — user/role/token CRUD + `Node::bootstrap_owner` / `Node::authenticate_token` / `Node::authorize_user` (the token-resolved capability decision, #745).
- `include/zetta/api/server.hpp` — the embedded `ApiServer` + `ServerOptions`; the auth gate is **unconditional** (#799 removed the `require_auth` and `bearer_token` options — a bearer that resolves to a user is required on every gated route).
- `src/core/api/http_server.cpp` — the ONE auth-gate middleware at the router boundary (`bearer → user`, else 401; #745).
- `tests/api/test_auth_gate.cpp` — the gate + `authorize_user` coverage (401 on every path without a token; the owner token opens them; per-user capability verdicts).
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
