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
scope.grant(zetta::ResourceKind::Database, "churn");   // a key scoped to one db

scope.permits({zetta::ResourceKind::Database, "churn"});  // true  — in scope
scope.permits({zetta::ResourceKind::Database, "fraud"});  // false — out of scope
scope.permits({zetta::ResourceKind::Project,  "churn"});  // false — wrong kind
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

`zetta::authorize(const AccessKey&, const Resource&)` is the pure decision over a
known key; `Node::authorize(key_id, resource)` is the seam a request path calls.

## The capability decision (ENT-22)

Capabilities answer a different question than scope: not *which resource* but
*which right*. An operation — a `(domain, action)` pair — is allowed **only** when
the key holds a **matching, unexpired** capability, and denied otherwise
(deny-by-default; there is no wildcard capability). A capability matches an
operation iff its `domain` and `action` are exactly equal, and its TTL (if any)
has not elapsed as of the time of the check.

```cpp
zetta::CapabilitySet caps;
caps.grant(zetta::CapabilityDomain::Data, "read");                  // an open-ended right
caps.grant(zetta::CapabilityDomain::Api, "deploy", "2026-07-01T00:00:00.000Z");  // TTL'd

caps.permits({CapabilityDomain::Data, "read"},  now);   // true  — matching right
caps.permits({CapabilityDomain::Data, "write"}, now);   // false — never granted
caps.permits({CapabilityDomain::Api,  "deploy"}, now);  // true iff now < the TTL
```

The two axes **compose** but stay independent: a full operation-on-a-resource
check asks both `authorize(key, resource)` (scope) and `authorize(key, operation,
now)` (capability), and proceeds only if both allow. Through the node:

```cpp
auto pipeline = node.create_principal("pipeline", PrincipalKind::Service);
zetta::CapabilitySet caps;
caps.grant(CapabilityDomain::Orchestration, "start_run");
auto key = pipeline->create_key(scope, caps);            // scope + capabilities

node.authorize(key->id(), Operation{CapabilityDomain::Orchestration, "start_run"});  // allowed
node.authorize(key->id(), Operation{CapabilityDomain::Data, "write"});               // denied
```

`zetta::authorize(const AccessKey&, const Operation&, std::string_view now)` is the
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
[`schemas/principal.schema.json`](../schemas/principal.schema.json) and
[`schemas/access_key.schema.json`](../schemas/access_key.schema.json) (the latter
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
without changing this seam.

## Pointers

- `include/zetta/principal.hpp` — `zetta::Principal` (ENT-20) + `PrincipalKind`.
- `include/zetta/access_key.hpp` — `zetta::AccessKey` (ENT-21), `AccessDecision`,
  `authorize()`.
- `include/zetta/scope.hpp` — `zetta::Scope` / `Resource` / `ResourceKind` (the
  deny-by-default *resource* decision).
- `include/zetta/capability.hpp` — `zetta::Capability` / `CapabilitySet` /
  `Operation` / `CapabilityDomain` (ENT-22; the deny-by-default *rights* decision,
  TTL'd).
- `include/zetta/node.hpp` — principal CRUD + `Node::authorize` (resource +
  operation overloads) / `find_access_key`.
- [replication.md](replication.md) — the CRDT model these entities replicate through.
- [projects.md](projects.md) — the sibling top-level entity whose
  capture/reconcile path this mirrors.
