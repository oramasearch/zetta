# Cloud docs

Provisioning compute — the **`Provisioner` seam** (provision N nodes against a capability spec /
tear down / list) and the backends behind it, from given-hardware to cloud burst. Every cloud
backend follows one recipe (orchestrator / control-plane seam / CLI binding / validated config — no
cloud SDK), mints short-lived burst identities so nodes join the cluster, and is **permission-gated,
deny-by-default** (the hard rule: nothing reads a provider's config or calls a cloud without
explicit per-action consent). Read here for anything touching node provisioning, cloud CLIs, burst
cost control, or the Add-nodes path; the GUI control is
[../gui/add-nodes-control.md](../gui/add-nodes-control.md).

- [provisioner.md](provisioner.md) — the `Provisioner` seam + node capability model, and the given-hardware `LocalProvisioner` backend (ARC-13 / ENT-32, #373/#374).
- [add-nodes.md](add-nodes.md) — the permanent-add control entry point: bring up N nodes that auto-join and persist as members; provision-only, never teardown (`zetta::cloud::add_nodes`, #518).
- [provisioner-detection.md](provisioner-detection.md) — detect cloud targets from the operator's own local CLIs (no credentials) → `system.provisioners` + injected provisioners.
- [node-bootstrap-env.md](node-bootstrap-env.md) — the node consumes the injected provisioning env on boot: `ZETTA_SEED_PEER`→`--peer`, `ZETTA_CLUSTER_TOKEN`→`--join-token`, `ZETTA_DATA_DIR`→`--data-dir`, as defaults beneath explicit flags, so a provisioned node auto-joins a secure cluster with no manual flags (`apply_bootstrap_env`, #749; resolves the env→flag half of Gap A); `ZETTA_CLUSTER_TOKEN_FILE` is the preferred token source, and the token is trimmed, validated against its source and scrubbed from the environment after reading (#770).
- [node-bootstrap-script.md](node-bootstrap-script.md) — the boot script a burst VM runs as its user-data: it carries only the *name* of the secret holding the join token, fetches it at boot from Key Vault / Secret Manager / OCI Vault / AWS SSM with the VM's identity, writes a 0400 token file and `/etc/zetta/bootstrap.env`, and starts `zetta.service` (`render_node_bootstrap_script`, #1316). The per-provider IAM is on each provider page.
- [burst-identity.md](burst-identity.md) — bulk-mint a short-lived service `Principal` + `AccessKey` per burst node, bulk-revoke on teardown, audited (`BurstIdentityManager`, #375).
- [../governance/audit-trail.md](../governance/audit-trail.md) — burst audit records ship through segment ranges without catalog event entries (#1164).
- [burst-approval-gate.md](burst-approval-gate.md) — the large-burst cost-cap approval gate: a `Provisioner` decorator that blocks over-cap bursts pending approval (APR-3 / RSK-9, #379).
- [cloud-provisioner-pattern.md](cloud-provisioner-pattern.md) — the recipe every cloud-burst backend follows: the four pieces, shared `CommandRunner`, the lifecycle→CLI mapping. CLI for now.
- [azure-provisioner.md](azure-provisioner.md) — the reference cloud-burst backend: VMSS / Spot via the `az` CLI, mint + teardown + revoke (`AzureProvisioner`, ADR-20, #376).
- [aws-provisioner.md](aws-provisioner.md) — the AWS mirror: EC2 Auto Scaling Group / Spot via the `aws` CLI (`AwsProvisioner`, #377).
- [gcp-provisioner.md](gcp-provisioner.md) — the GCP mirror: Managed Instance Group / Spot via the `gcloud` CLI (`GcpProvisioner`, #378).
- [oci-provisioner.md](oci-provisioner.md) — the OCI mirror: Instance Pool / preemptible via the `oci` CLI, OCID-resolved by name (`OciProvisioner`, #432).
- [k8s-provisioner.md](k8s-provisioner.md) — the Kubernetes mirror: burst worker pods via a `kubectl`-scaled Deployment into an existing cluster (`K8sProvisioner`, #434).
- [bare-metal-provisioner.md](bare-metal-provisioner.md) — the local-host mirror: burst worker containers via `docker`/`podman`; bursts where `LocalProvisioner` only reports (#436).
- [cloud-inspector.md](cloud-inspector.md) — consent-gated, read-only inspection of a deployment target: identity / regions / nodes, surfaced in the panel + `system.cloud_*` tables (#445).
