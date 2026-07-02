# Provisioner detection (wiring cloud targets from the local CLI)

The headed **Add nodes** control is deny-by-default: a cloud target's **Allow**
button only appears once a configured [`Provisioner`](provisioner.md) has been
injected for it (`App::set_provisioner`). Constructing a real one needs the
operator's subscription / region / resource group — but **not** their
credentials, because the reference CLI control plane (`AzCliControlPlane`, …)
drives the provider's own local CLI (`az`, `aws`, `gcloud`, `oci`, `kubectl`,
`docker`), which already carries its auth.

So zetta sources that configuration **from the local CLI itself**, not from a
file: at startup it asks each installed, logged-in CLI for its active deployment
context, records the result on the node, surfaces it as the
**`system.zetta_provisioners`** table, and wires a CLI-backed provisioner per
detected target into the Add-nodes control. This is the credential-free sibling
of the consent-gated [Cloud Inspector](cloud-inspector.md), which reads the same
CLIs.

## The flow

```
main() ── detect_provisioner_configs()         # run the local CLIs (read-only)
   │         → [ProvisionerConfig{target, cli, summary, config}]
   ├── node.record_provisioner_config(...)      # node-local state (no CRDT, no disk)
   │         → system.zetta_provisioners         # the SQL read view (both modes)
   └── (headed) CloudProvisioners::wire(app, node.provisioner_configs(), seed)
             → App::set_provisioner("azure", …)  # the Allow button lights up
```

- **`detect_provisioner_configs(CommandRunner = {})`**
  (`include/zetta/provisioner_detect.hpp`, `src/core/provisioner_detect.cpp`) runs
  each detector and returns what was found. Every detector runs a couple of fast,
  read-only, non-interactive CLI commands through the shared
  [`CommandRunner`](provisioner.md) seam (so it is testable against a fake and
  never touches a real cloud in CI). A CLI that is absent, not logged in, or
  returns nothing yields nothing for that provider — never an error.
- **`ProvisionerConfig`** (`include/zetta/provisioner_config.hpp`) is the detected
  context: `target` (catalog id), `cli`, a one-line `summary`, and the provider's
  own validated `XConfig::to_json()` shape in `config`. It carries **no
  credentials**.
- **`Node::record_provisioner_config` / `provisioner_configs()`**
  (mirrors `record_cloud_inspection`) hold it as **node-local, observed state** —
  not replicated, not persisted (RULE-7), because it reflects *this* node's CLI
  auth + defaults. Latest config per target wins.
- **`system.zetta_provisioners`** (`src/core/query/executor.cpp`) is the read view:
  one row per detected target — `target`, `cli`, `subscription` / `project` /
  `region` / `resource_group` / `pool` / `spot`, `summary`. Populated in **both**
  headed and headless modes.
- **`CloudProvisioners::wire`** (`src/gui/cloud_wiring.hpp/.cpp`) rebuilds a real
  CLI-backed provisioner from each detected config — owning the control plane +
  provisioner for the App's lifetime — and `set_provisioner`s it. A target with no
  detected config stays unwired (the gate then reads "no provisioner wired",
  unchanged). The cluster's reachable address is injected as the burst seed so new
  nodes auto-join.

The actual add stays **per-action consent-gated** in the GUI and is never
remembered — detection only makes the Allow button *available*; clicking it is
still the one explicit, billable write.

## Azure (the reference detector)

`detect_azure_provisioner` reads:

- `az account show -o json` → `subscription_id` (+ `tenantId`). A non-zero exit
  (CLI absent / not logged in) ⇒ not detected.
- `az config get defaults.group` / `defaults.location` → the operator's configured
  resource group + region, falling back to a stable `zetta-burst` group +
  `eastus` when unset — so a logged-in `az` always yields a valid, wireable config.
  An operator who wants different targeting sets
  `az configure --defaults group=… location=…` (visible in
  `system.zetta_provisioners`).

## Extending to the other targets

AWS / GCP / OCI / Kubernetes / bare-metal follow the same shape — their CLI's
`whoami` / `config` → their validated `XConfig` — and slot into
`detect_provisioner_configs` independently; one CLI being absent never blocks the
others. `CloudProvisioners` already owns all six provisioner slots, so adding a
detector lights up its target with no other change.

## Tests

- `tests/test_provisioner_detect.cpp` — the Azure detector against a fake
  `CommandRunner` (logged-in, no-defaults fallback, CLI-missing, malformed output).
- `tests/test_query.cpp::SelectsProvisionersSystemTable` — `system.zetta_provisioners`
  reflects recorded configs (and latest-per-target wins).

## See also

- [add-nodes-control.md](add-nodes-control.md) — the GUI control this lights up.
- [cloud-inspector.md](cloud-inspector.md) — the read-only CLI sibling.
- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the per-provider
  CLI control-plane pattern the wired provisioners use.
