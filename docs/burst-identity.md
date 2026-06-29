# Bulk burst identity + teardown

When the cluster **bursts** compute nodes for a job — standing up extra nodes
through the [`Provisioner`](provisioner.md) seam — each burst node needs an
identity to authenticate to the cluster. The **`BurstIdentityManager`**
(`include/zetta/burst_identity.hpp`) mints those identities **in bulk on
provision** and **revokes them in bulk on teardown**, by **reusing the existing
identity model** ([`Principal` + `AccessKey`](access-control.md), `#304`) — no new
identity system. It realizes **ARC-13** and mitigates **RSK-9** (long-lived,
orphaned burst credentials) for the native distributed accelerated-compute pass
of UC-1 churn (`#264`, `#375`).

> **Scope, deliberately.** This slice is the **identity lifecycle** — bulk mint /
> bulk revoke / list, audited. It does not stand nodes up itself (that is the
> [`Provisioner`](provisioner.md) backends, `#374` / `#377` / `#378`) and does not
> get the key onto the burst node (the bootstrap/handshake is a later slice). The
> autoscaler / job coordinator wires the two together: provision, mint, run, then
> revoke + tear down.

## What it mints

For each burst node, one **service** `Principal` + one `AccessKey`:

- The principal is a `PrincipalKind::Service` actor named
  **`burst/<job-id>/<node-id>`** (the naming convention is load-bearing — see
  [below](#no-parallel-state-the-job-identity-link-is-derived)).
- The key is **scoped to the job**: the caller passes a `Scope` naming the job's
  resources (e.g. its project) and a `CapabilitySet` of the rights the node needs.
- The key is **short-lived**: TTL the capabilities (`Capability::expires_at`, see
  [access-control.md](access-control.md)) so the grant expires with the burst.

A `BurstIdentity` is the handle returned per node — the credential the burst node
authenticates with for the life of the job:

```cpp
struct BurstIdentity {
    std::string node_id;       // the provisioned burst node this identity serves
    std::string principal_id;  // the minted service Principal's id
    std::string key_id;        // the minted AccessKey's id (scoped to the job, TTL'd)
};
```

## The manager

```cpp
class BurstIdentityManager {
public:
    explicit BurstIdentityManager(Node& node);

    std::vector<BurstIdentity> mint(const std::string& job_id,
                                    const std::vector<ProvisionedNode>& nodes,
                                    const Scope& scope,
                                    const CapabilitySet& capabilities = {});
    std::size_t revoke(const std::string& job_id);
    std::vector<BurstIdentity> minted(const std::string& job_id) const;
};
```

- **`mint`** — bulk-mint one identity per node. Records a `key_issued` audit event
  per **newly** minted identity. **Idempotent**: a node already minted for this job
  is returned as-is (no duplicate principal, no second audit event), so a
  re-provision of the same node is safe.
- **`revoke`** — bulk-revoke every identity minted for `job_id`: drop each service
  principal (which drops its key), recording a `key_revoked` audit event per one.
  **Idempotent** — revoking an unknown / already-revoked job is a no-op returning
  `0`. Returns the number revoked.
- **`minted`** — the identities currently up for `job_id`, derived from the node's
  principals (empty for an unknown job).

### Pairing with the Provisioner seam

```cpp
FakeProvisioner provisioner{/*capacity=*/8};
BurstIdentityManager identities{node};

// Provision a burst, then bulk-mint a job-scoped identity for each node.
auto burst = provisioner.provision(ProvisionRequest{3, gpu_spec});
Scope scope;                                   // scope each key to the job's project
scope.grant(ResourceKind::Project, "churn");
CapabilitySet caps;                            // TTL'd → short-lived
caps.grant(CapabilityDomain::Orchestration, "run", expires_at);
auto minted = identities.mint(job_id, burst.nodes, scope, caps);   // -> key_issued ×3

// ... run the job on the burst nodes ...

// Tear the burst down: bulk-revoke the identities, then release the nodes.
identities.revoke(job_id);                                         // -> key_revoked ×3
for (const auto& node : provisioner.provisioned()) provisioner.teardown(node.node_id);
```

### Wired automatically: the cloud-burst `IdentityHooks`

The example above mints + revokes by hand. The **production ephemeral-burst path**
(ADR-20) wires the two edges into the cloud-burst provisioner itself so they fire
automatically. Every cloud backend
([`AzureProvisioner`](azure-provisioner.md), AWS, GCP, OCI, k8s, bare-metal) is a
[`BurstProvisioner`](cloud-provisioner-pattern.md) carrying an optional
**`IdentityHooks`** pair (`include/zetta/burst_provisioner.hpp`): `mint` is invoked
with the nodes a `provision` brought up, and `revoke` is invoked once, when the
burst's **last** node is torn down (the pool drains). Bind them to a
`BurstIdentityManager` for the job and the whole lifecycle runs hands-off:

```cpp
BurstIdentityManager identities{node};
AzureProvisioner::IdentityHooks hooks;
hooks.mint   = [&](const std::vector<ProvisionedNode>& nodes) {
    identities.mint(job_id, nodes, scope, caps);      // provision -> key_issued ×N
};
hooks.revoke = [&] { identities.revoke(job_id); };    // last teardown -> key_revoked ×N
AzureProvisioner provisioner{control_plane, config, hooks};

provisioner.provision(ProvisionRequest{N, spec});     // brings up N + mints N identities
// ... run the job ...
for (const auto& live : provisioner.provisioned()) provisioner.teardown(live.node_id);
// the final teardown drains the pool -> revoke fires once -> no orphaned identities
```

Because `revoke` is **bulk** (drops every `burst/<job-id>/…` principal) and fires
exactly on the transition to an empty pool, a fully torn-down burst leaves **no
orphaned credentials** — the property TC-6 (`#253`) verifies.

## No parallel state: the job↔identity link is derived

zetta's core rule is **everything is replicated state, and in-memory structures
are *derived* from the CRDT document — never a parallel copy** (see
[replication.md](replication.md)). The manager honors this by **not** keeping its
own job→credentials map. Instead it encodes the association in the service
principal's **name** — `burst/<job-id>/<node-id>` — so the set of identities for a
job is exactly the principals whose name carries that prefix.

`Principal`s and `AccessKey`s are ordinary replicated state, so:

- a **mint** replicates + persists like any other identity (the `principal:` /
  `key:` CRDT keys, the on-disk projection),
- a **revoke** removes it from the document + disk on every node,
- and `minted()` / `revoke()` keep working **after a restart** — they read the
  identities straight back off the reconstructed principal tree, because nothing
  lives outside it.

## Audit

Both lifecycle edges are audited via the Node's [`AuditSink`](audit-trail.md). The
[`AuditAction`](audit-trail.md) taxonomy gains two governed actions:

| Action | When | `target` | `detail` |
|---|---|---|---|
| `key_issued` | a burst identity is minted | the access key id | `burst identity minted for job <id> on node <id>` |
| `key_revoked` | a burst identity is revoked | the access key id | `burst identity revoked for job <id> on node <id>` |

The events are emitted only for the **local** mint/revoke action — the reverse
`materialize_*` reconciliation paths never emit, so a peer's burst identity arrives
already-audited as its own replicated `audit:` event. The two new actions are part
of the AuditEvent JSON Schema (the inline source of truth in
`src/core/audit_event.cpp`, mirrored by
[`schemas/audit_event.schema.json`](../schemas/audit_event.schema.json)), so a
malformed action still fails validation at the boundary. These same `key_issued` /
`key_revoked` actions are reusable by the future "grant a capability as a
first-class event" FEAT-9 slice.

## Tests

`tests/test_burst_identity.cpp` covers the acceptance criterion (bulk-mint N,
bulk-revoke on teardown, audit events recorded) against the real `Provisioner`
seam (a `FakeProvisioner` like the one in `tests/test_provisioner.cpp`):

- **bulk mint → bulk revoke, audited** — provision 3, mint 3 (one service
  principal + one job-scoped key each, 3 `key_issued` events), then `revoke` all 3
  (principals/keys gone, 3 `key_revoked` events) and tear the nodes down; revoking
  again is a no-op.
- **the full burst lifecycle through the real provisioner (TC-6)** — wire the real
  [`AzureProvisioner`](azure-provisioner.md)'s `IdentityHooks` to a real
  `BurstIdentityManager` and burst fleets of 1 / 3 / 8: provisioning mints N
  **distinct** identities (distinct nodes, principals, keys; N `key_issued`), no
  identity is revoked until the **last** teardown drains the pool, and a fully
  torn-down burst leaves **no orphans** (0 principals, N `key_revoked`). This proves
  provision *triggers* the mint and teardown *triggers* the bulk revoke — the ADR-20
  / RSK-9 acceptance — rather than calling mint/revoke by hand.
- **service + job scope** — the minted principal is a `Service` actor, its key
  permits the job's project and denies others, and the TTL'd capability is denied
  once `now` passes `expires_at`.
- **idempotent mint** — re-minting the same (job, node) returns the same
  principal + key and audits only once.
- **survives restart** — a fresh node rebuilds the principal tree from disk;
  `minted()` and `revoke()` still find the burst identity (proving it is derived,
  not separately persisted).
- **the new audit actions** round-trip through `to_string` / `audit_action_from`
  and pass schema validation.

The same acceptance is a **runnable, parameterized in-app case**,
`governance.burst_identity` in the [test suite](test-suite.md) catalog
(`zetta::testkit`) — burst N nodes (a tunable burst-size option) through the real
provisioner ↔ manager wiring and assert N distinct identities minted + revoked with
no orphans, both headless (`tests/test_testkit.cpp`) and from the headed
**Operations → Test suite** panel.

## Pointers

- `include/zetta/burst_identity.hpp` — `zetta::BurstIdentity` /
  `BurstIdentityManager`.
- `src/core/burst_identity.cpp` — bulk mint / revoke / list over the Node's
  principal CRUD + audit sink.
- [provisioner.md](provisioner.md) — the `Provisioner` seam these identities are
  minted for (the source of burst nodes).
- [access-control.md](access-control.md) — the `Principal` + `AccessKey` + `Scope`
  + `Capability` model reused here.
- [audit-trail.md](audit-trail.md) — the immutable trail the `key_issued` /
  `key_revoked` events append to.
- [replication.md](replication.md) — the "everything is replicated, derive don't
  duplicate" rule the derived job↔identity link follows.
