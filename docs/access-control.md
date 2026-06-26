# Access control (key-scoped, V1)

zetta's V1 access model is **key-scoped**: a request authenticates with an
**access key**, and thereby inherits exactly the data access that key permits —
in-scope resources are allowed, everything else is denied. This realizes the
business rule **RULE-3** / permission rule **PERM-1** and the decision **ADR-6**
(*operate over exactly what the login key permits; team/partner/per-dataset tiers
later*), and implements the closed domain entities **ENT-20 (Principal)** and
**ENT-21 (AccessKey)**. It is the first slice of **FEAT-9 (Governed
self-service)**.

> **V1 scope, deliberately.** There is **no bespoke permission model** yet — a
> key's scope *is* the access. Finer tiers (team / partner / per-dataset, and
> per-table / per-row granularity) are explicitly deferred; the audit trail and
> approval gates are separate FEAT-9 slices. This slice is the types + the
> enforcement decision, wired into the node and replicated.

## The shape

```
Principal  (ENT-20)   an authenticated actor — user | service | node
  id, kind, name, created

AccessKey  (ENT-21)   a login credential owned by a principal
  id, principal, scope, created          (immutable once created)

Scope                 what a key permits — deny-by-default
  unrestricted  bool                     the "*" grant (an admin key)
  grants        [ Resource ]             the explicit in-scope resources

Resource              a node-owned resource a grant names / a request targets
  kind  database | project               (coarse, top-level — finer tiers later)
  name  the resource's handle
```

A **Principal** is an authenticated actor — the subject of access grants and
(in a later slice) the audit trail. A principal owns its **access keys** the same
way a `Project` owns its inputs or a `Database` owns its tables. Access itself is
decided by the *key's scope*, not by the principal's kind.

An **AccessKey** is the login credential whose scope bounds a principal's reach.
A key is an **immutable credential**: it is created once with a fixed scope and
never edited — to change access, issue a new key and drop the old one.

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

## Replication + persistence

Principals and keys are **replicated state**, like everything else in zetta (the
Automerge CRDT document is the source of truth; the on-disk tree and in-memory
node are its projection). They use flat catalog keys (`src/core/catalog.hpp`):

```
principal:<uuid>  ->  { kind, name, created }
key:<uuid>        ->  { principal, scope: { unrestricted, grants:[...] }, created }
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
      key.json              { id, principal, scope, created }
```

The folder name *is* the id and never changes; rename rewrites `principal.json`.
A key carries no body — the scope is the whole record — so unlike a project input
there is nothing to transfer peer-to-peer; the `key:` value replicates in full.

## JSON

`Principal` metadata and `AccessKey` metadata (with its embedded `Scope`)
round-trip through JSON via the nlohmann ADL `to_json` hooks, and are parsed at
the trust boundary with `scope_from_json()` (and the shape checks in
`Principal::load()` / `Node::load_principals()`), which **reject anything
malformed** — not an object, a missing or wrong-typed field, an unknown resource
kind, a non-array `grants`. This matters most for the **scope**: a malformed
scope must fail fast rather than silently widen or narrow access, so a corrupt
`key.json` is skipped at load instead of granting the wrong reach.

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
  "created": "2026-06-25T14:03:21.123Z"
}
```

## Where it fits

This is the foundation the rest of FEAT-9 builds on. The **audit trail** (every
graph change / deploy / data access) is now built — each `Node::authorize`
decision records an immutable `data_access` [`AuditEvent`](audit-trail.md). A
later slice threads the login key through the request path (the query executor,
the cluster protocol) so every data access is checked against `Node::authorize`
(and thereby audited), not just the seam itself; **approval gates** for risky
actions are their own slice. The finer access **tiers** (team / partner /
per-dataset) extend `ResourceKind` and `Scope` without changing this seam.

## Pointers

- `include/zetta/principal.hpp` — `zetta::Principal` (ENT-20) + `PrincipalKind`.
- `include/zetta/access_key.hpp` — `zetta::AccessKey` (ENT-21), `AccessDecision`,
  `authorize()`.
- `include/zetta/scope.hpp` — `zetta::Scope` / `Resource` / `ResourceKind` (the
  deny-by-default decision).
- `include/zetta/node.hpp` — principal CRUD + `Node::authorize` / `find_access_key`.
- [replication.md](replication.md) — the CRDT model these entities replicate through.
- [projects.md](projects.md) — the sibling top-level entity whose
  capture/reconcile path this mirrors.
