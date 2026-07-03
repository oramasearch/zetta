# Burst identity + teardown (TC-6) · `governance.burst_identity`

**Category:** Governance · **Source:** `src/core/testkit/cases_governance.cpp` · **Engine doc:** [burst-identity.md](../../../cloud/burst-identity.md)

The bulk-burst-identity lifecycle (TC-6 / RSK-9). Bursts N nodes through the real
cloud-burst provisioner wired to a [`BurstIdentityManager`](../../../cloud/burst-identity.md):
provisioning mints N distinct short-lived service identities, and tearing the
burst down revokes them in bulk with no orphans left behind.

## What it does

1. **Wires a real burst.** Starts a node and a `BurstIdentityManager` over it,
   then drives the **real** `AzureProvisioner` (the reference ADR-20
   ephemeral-burst backend) against a fake control plane modelling the pool, so
   the lifecycle runs with no cloud SDK / CLI. The provisioner's `IdentityHooks`
   bind mint-on-provision + bulk-revoke-on-teardown to the manager.
2. **Bursts N nodes → mints N identities.** Provisions `nodes` burst nodes; each
   gets one short-lived service [`Principal` + `AccessKey`](../../../governance/access-control.md),
   scoped to the job's `churn` project with a TTL'd `orchestration:run` right (so
   the identity is both narrow and expiring). Each mint records a `key_issued`
   [audit event](../../../governance/audit-trail.md).
3. **Checks the identities are distinct.** Confirms one identity per node, with
   distinct node ids, distinct principals, and distinct keys (no two burst nodes
   share a credential).
4. **Tears the burst down → revokes them.** Tears down every live node, draining
   the pool, which bulk-revokes the identities — each revoke records a
   `key_revoked` audit event.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Governance → Burst identity + teardown (TC-6)**. Set the burst size in
the modal and click **Run**; the result pane shows pass/fail, the metrics (burst
nodes, distinct principals/keys, `key_issued` / `key_revoked`, orphans), and the
log.

**Headless:** the case is exercised (at `nodes=4`) by
`tests/testkit/test_testkit.cpp::GovernanceCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.GovernanceCasesPass' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–16 | how many nodes to burst (one identity each) |

## What it asserts (pass criteria)

- **One identity per burst node** — `minted` size equals `nodes`, with distinct
  node ids, principals, and keys.
- **N service principals + N `key_issued`** audit events after the burst.
- **No orphans on teardown** — zero principals and zero minted identities remain
  after draining the pool.
- **N `key_revoked`** audit events on teardown.

## Related

- Engine: [burst-identity.md](../../../cloud/burst-identity.md) ·
  [access-control.md](../../../governance/access-control.md) ·
  [audit-trail.md](../../../governance/audit-trail.md) ·
  [azure-provisioner.md](../../../cloud/azure-provisioner.md)
- Sibling cases: [`governance.access_control`](governance.access_control.md) ·
  [`governance.audit`](governance.audit.md) ·
  [`governance.entity_resolution_gate`](governance.entity_resolution_gate.md)
