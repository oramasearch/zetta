# Research: OCI CLI Compute Guide

> **Provenance.** Captured 2026-06-26 from a user-supplied hands-on guide
> ("OCI CLI Compute Guide: Launching Workloads with `oci`"), whose command syntax
> is grounded in the official Oracle Cloud Infrastructure CLI / Compute docs
> (primary-source links are inline below).
>
> **Status:** reference / background. It is the cited basis for the `oci` command
> shapes the **OCI cloud-burst provisioner** uses — `OciCliControlPlane`
> (`src/core/cloud/oci_cli_control_plane.cpp`); see
> [oci-provisioner.md](../cloud/oci-provisioner.md). zetta's burst backend uses only the
> **Instance Pool + preemptible** slice (§2.8 Preemptible instances, §2.10
> Instance pools + autoscaling); the rest (single VMs, Container Instances, OKE,
> Functions, GPU/HPC) is background for choosing primitives and for the
> Azure/GCP/AWS equivalents in
> [cloud-provisioner-pattern.md](../cloud/cloud-provisioner-pattern.md). Examples are
> illustrative — replace compartment/subnet/image OCIDs, availability domains,
> shapes, and regions with values your tenancy allows.

---

# OCI CLI Compute Guide: Launching Workloads with `oci`

This guide is a practical, command-first walkthrough for launching workloads on Oracle Cloud Infrastructure (OCI) compute services with the OCI CLI, from single virtual machines to container instances, OKE Kubernetes clusters, Functions, GPU, and HPC workloads. The OCI CLI is Oracle Cloud Infrastructure's command-line interface, and its global options include `--profile`, `--region`, `--auth`, `--query`, `--raw-output`, and `--cli-rc-file` ([OCI CLI command reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/oci.html)).

> Replace all placeholder OCIDs such as `ocid1.compartment.oc1..aaaa...`, `ocid1.subnet.oc1..aaaa...`, and `ocid1.image.oc1..aaaa...` with values from your tenancy. OCI resource creation commands commonly require a compartment OCID, and many commands also require availability domain names, subnet OCIDs, image OCIDs, VCN OCIDs, or cluster OCIDs ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

---

## 0. The OCI mental model

| Concept | Practical meaning for CLI users |
| --- | --- |
| Tenancy | The root container for your organization's OCI resources; Oracle describes a tenancy as the root compartment containing all of an organization's OCI resources ([IAM overview](https://docs.oracle.com/iaas/Content/Identity/Concepts/overview.htm)). |
| Compartment | A logical container used to organize resources for access control, billing, and isolation, and most compute commands require `--compartment-id` / `-c` ([IAM overview](https://docs.oracle.com/iaas/Content/Identity/Concepts/overview.htm)). |
| OCID | Oracle-assigned unique identifier for resources; Oracle notes that most OCI resources have a unique Oracle Cloud ID, or OCID ([IAM overview](https://docs.oracle.com/iaas/Content/Identity/Concepts/overview.htm)). |
| Region | A geographic OCI region such as `us-ashburn-1`, selected by config, environment variable, or `--region` ([OCI CLI command reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/oci.html)). |
| Availability domain | A data center grouping inside a region; compute instances and container instances often need an availability domain name such as `Uocm:PHX-AD-1` ([availability-domain list reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/iam/availability-domain/list.html)). |
| Shape | The hardware template for an instance or node, determining CPU, memory, networking, GPU, and local storage characteristics ([Compute overview](https://docs.oracle.com/iaas/Content/Compute/Concepts/computeoverview.htm)). |
| Image | The OS boot image used for instances; platform image lists are refreshed, so list and capture the current image ID instead of hard-coding stale image OCIDs ([image list reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/image/list.html)). |

Recommended shell setup:

```bash
# Required: choose your compartment and region.
export COMPARTMENT_ID="ocid1.compartment.oc1..aaaa..."
export REGION="us-ashburn-1"

# Optional: choose a profile from ~/.oci/config.
export OCI_CLI_PROFILE="DEFAULT"

# Optional: make JSON output easier to work with.
alias oci='oci --region "$REGION"'
```

OCI's CLI supports JMESPath queries with `--query` and raw text output with `--raw-output`, which makes shell variable capture practical ([OCI CLI command reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/oci.html)). Example:

```bash
AD=$(oci iam availability-domain list \
  --compartment-id "$COMPARTMENT_ID" \
  --query 'data[0].name' \
  --raw-output)

echo "$AD"
```

---

## 1. Getting started with OCI CLI

### 1.1 Install the CLI

On Oracle Linux 9, Oracle documents installing the CLI with `dnf` by enabling `oraclelinux-developer-release-el9` and installing `python39-oci-cli` ([OCI CLI quickstart](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/cliinstall.htm)).

```bash
sudo dnf -y install oraclelinux-developer-release-el9
sudo dnf install -y python39-oci-cli
oci --version
```

On Linux and UNIX systems, Oracle documents the installer script approach with `install.sh` ([OCI CLI quickstart](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/cliinstall.htm)).

```bash
bash -c "$(curl -L https://raw.githubusercontent.com/oracle/oci-cli/master/scripts/install/install.sh)"
oci --version
```

On Windows, Oracle documents an MSI installer and a PowerShell installer, including downloading `install.ps1` from the OCI CLI GitHub repository ([OCI CLI quickstart](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/cliinstall.htm)).

```powershell
Set-ExecutionPolicy RemoteSigned
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest https://raw.githubusercontent.com/oracle/oci-cli/master/scripts/install/install.ps1 -OutFile install.ps1
./install.ps1 -AcceptAllDefaults
oci --version
```

Oracle also publishes a container image with the OCI CLI preinstalled, which can be run by mounting `$HOME/.oci` into `/oracle/.oci` inside the container ([OCI CLI container image docs](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/clicontainer.htm)).

```bash
docker pull ghcr.io/oracle/oci-cli:latest
docker run --rm -it -v "$HOME/.oci:/oracle/.oci" ghcr.io/oracle/oci-cli:latest os ns get
```

### 1.2 Configure API-key authentication with `oci setup config`

The standard API-key setup uses `oci setup config`, which prompts for the config file path, user OCID, tenancy OCID, region, and API signing key generation details ([Functions CLI setup tutorial](https://docs.oracle.com/en-us/iaas/Content/developer/functions/func-setup-cli/01-summary.htm)).

```bash
oci setup config
```

A typical `~/.oci/config` profile looks like this:

```ini
[DEFAULT]
user=ocid1.user.oc1..aaaa...
fingerprint=12:34:56:78:90:ab:cd:ef:12:34:56:78:90:ab:cd:ef
tenancy=ocid1.tenancy.oc1..aaaa...
region=us-ashburn-1
key_file=~/.oci/oci_api_key.pem
```

After `oci setup config` generates a public key such as `~/.oci/oci_api_key_public.pem`, upload that public key to your OCI user under API Keys before making API calls; Oracle's Functions CLI tutorial explicitly walks through copying the generated public key and adding it to the user account ([Functions CLI setup tutorial](https://docs.oracle.com/en-us/iaas/Content/developer/functions/func-setup-cli/01-summary.htm)).

```bash
cat ~/.oci/oci_api_key_public.pem
```

Verify the profile with a low-risk call:

```bash
oci iam region list --output table
oci os ns get
```

`oci iam region list` is the documented way to list valid region names for the `--region` option, and `oci os ns get` is used in Oracle setup examples to verify CLI authentication ([OCI CLI command reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/oci.html), [Functions CLI setup tutorial](https://docs.oracle.com/en-us/iaas/Content/developer/functions/func-setup-cli/01-summary.htm)).

### 1.3 Session-token authentication

For browser-based temporary authentication without a long-lived API signing key, Oracle documents `oci session authenticate` ([token-based authentication docs](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/clitoken.htm)).

```bash
oci session authenticate --region us-ashburn-1 --profile-name SESSION_IAD
```

Use the generated session profile with `--auth security_token`, or set `OCI_CLI_AUTH=security_token`, because Oracle states that session-token CLI commands must use `--auth security_token` or the matching environment variable ([token-based authentication docs](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/clitoken.htm)).

```bash
oci os ns get --profile SESSION_IAD --auth security_token

export OCI_CLI_PROFILE="SESSION_IAD"
export OCI_CLI_AUTH="security_token"
oci iam region list
```

Refresh an existing session token with `oci session refresh` ([token-based authentication docs](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/clitoken.htm)).

```bash
oci session refresh --profile SESSION_IAD
```

### 1.4 Instance principals

Instance principals let an OCI compute instance call OCI APIs without storing user API keys on the instance; Oracle describes dynamic groups as groups of resources, such as compute instances, that can act as principal actors and make API calls according to policies ([IAM overview](https://docs.oracle.com/iaas/Content/Identity/Concepts/overview.htm)).

```bash
# Run this from an OCI Compute instance that belongs to a dynamic group
# with policies granting the required permissions.
oci os ns get --auth instance_principal

# Or set it for the shell session.
export OCI_CLI_AUTH=instance_principal
oci compute instance list --compartment-id "$COMPARTMENT_ID"
```

Oracle documents `--auth instance_principal` and the `OCI_CLI_AUTH=instance_principal` environment variable for instance principal authorization ([instance principal docs](https://docs.oracle.com/en/engineered-systems/private-cloud-appliance/3.0-latest/user/user-usr-instance-principals.html)).

### 1.5 Default profile, region, and command defaults

Oracle documents setting a default profile in `~/.oci/oci_cli_rc` under `[OCI_CLI_SETTINGS]`, and the precedence is `--profile`, then `OCI_CLI_PROFILE`, then `default_profile` ([CLI configuration docs](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/cliconfigure.htm)).

```bash
oci setup oci-cli-rc --file ~/.oci/oci_cli_rc
cat >> ~/.oci/oci_cli_rc <<'EOF'
[OCI_CLI_SETTINGS]
default_profile=DEFAULT

[DEFAULT]
compartment-id=ocid1.compartment.oc1..aaaa...
EOF
```

You can always override profile and region per command with global flags, and the OCI CLI command reference documents `--profile` and `--region` as global options ([OCI CLI command reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/oci.html)).

```bash
oci --profile PROD --region us-phoenix-1 os ns get
```

---

## 2. Compute Instances: virtual machines and bare metal

OCI Compute provides bare metal instances and VM instances; Oracle describes bare metal as dedicated physical server access and VMs as isolated environments running on top of physical bare metal hardware ([Compute overview](https://docs.oracle.com/iaas/Content/Compute/Concepts/computeoverview.htm)).

### 2.1 Discover availability domains, images, and shapes

List availability domains in the tenancy or a compartment; Oracle documents `oci iam availability-domain list` and notes that `--compartment-id` can be the tenancy OCID or another compartment OCID ([availability-domain list reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/iam/availability-domain/list.html)).

```bash
oci iam availability-domain list \
  --compartment-id "$COMPARTMENT_ID" \
  --output table

AD=$(oci iam availability-domain list \
  --compartment-id "$COMPARTMENT_ID" \
  --query 'data[0].name' \
  --raw-output)
```

List recent platform and custom images in your compartment; Oracle documents `oci compute image list` with required `--compartment-id` ([image list reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/image/list.html)).

```bash
oci compute image list \
  --compartment-id "$COMPARTMENT_ID" \
  --operating-system "Oracle Linux" \
  --operating-system-version "9" \
  --sort-by TIMECREATED \
  --sort-order DESC \
  --all \
  --query 'data[0:5].{name:"display-name",id:id,created:"time-created"}' \
  --output table

IMAGE_ID=$(oci compute image list \
  --compartment-id "$COMPARTMENT_ID" \
  --operating-system "Oracle Linux" \
  --operating-system-version "9" \
  --sort-by TIMECREATED \
  --sort-order DESC \
  --all \
  --query 'data[0].id' \
  --raw-output)
```

List shapes available in a compartment and optionally filter by availability domain and image compatibility; Oracle documents `oci compute shape list` with required `--compartment-id` and optional `--availability-domain` and `--image-id` ([shape list reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/shape/list.html)).

```bash
oci compute shape list \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --image-id "$IMAGE_ID" \
  --all \
  --query 'data[?contains(shape, `Flex`)].{shape:shape, ocpus:"ocpus", memory:"memory-in-gbs"}' \
  --output table
```

### 2.2 Find or create networking

Compute instance launch requires a subnet OCID, and the launch command reference lists `--subnet-id` as a required parameter ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)). If you already have a subnet, capture it like this:

```bash
VCN_ID=$(oci network vcn list \
  --compartment-id "$COMPARTMENT_ID" \
  --query 'data[0].id' \
  --raw-output)

SUBNET_ID=$(oci network subnet list \
  --compartment-id "$COMPARTMENT_ID" \
  --vcn-id "$VCN_ID" \
  --query 'data[0].id' \
  --raw-output)
```

For a quick test VCN and subnet, create them with simple CIDRs; Oracle's compute launch example creates a VCN and subnet before launching an instance ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

```bash
VCN_ID=$(oci network vcn create \
  --compartment-id "$COMPARTMENT_ID" \
  --display-name "cli-demo-vcn" \
  --cidr-block "10.0.0.0/16" \
  --dns-label "clidemo" \
  --query 'data.id' \
  --raw-output)

SUBNET_ID=$(oci network subnet create \
  --compartment-id "$COMPARTMENT_ID" \
  --vcn-id "$VCN_ID" \
  --display-name "cli-demo-public-subnet" \
  --cidr-block "10.0.1.0/24" \
  --dns-label "public1" \
  --query 'data.id' \
  --raw-output)
```

> A production subnet also needs route tables, gateways, security lists or network security groups, DNS choices, and least-privilege ingress rules; the simple commands above are a starting point for command syntax, not a complete production network design.

### 2.3 Launch a VM instance

`oci compute instance launch` creates an instance in a specified compartment and availability domain, and its required parameters include `--availability-domain`, `--compartment-id`, and `--subnet-id` ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

```bash
ssh-keygen -t ed25519 -f ~/.ssh/oci_demo_ed25519 -N ""

INSTANCE_ID=$(oci compute instance launch \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "cli-demo-vm" \
  --shape "VM.Standard.E5.Flex" \
  --shape-config '{"ocpus": 1, "memoryInGBs": 8}' \
  --image-id "$IMAGE_ID" \
  --subnet-id "$SUBNET_ID" \
  --assign-public-ip true \
  --ssh-authorized-keys-file ~/.ssh/oci_demo_ed25519.pub \
  --freeform-tags '{"project":"oci-cli-guide","owner":"platform"}' \
  --wait-for-state RUNNING \
  --query 'data.id' \
  --raw-output)

echo "$INSTANCE_ID"
```

The `--shape` flag specifies the instance shape, and Oracle states that a shape determines the number of CPUs, memory, and other resources allocated to the instance ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)). The `--shape-config` flag is used for flexible shapes, and Oracle describes flexible shapes as shapes where you customize the number of OCPUs and amount of memory ([Compute overview](https://docs.oracle.com/iaas/Content/Compute/Concepts/computeoverview.htm)). The `--ssh-authorized-keys-file` flag is a convenience wrapper for the `ssh_authorized_keys` metadata field ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

### 2.4 Get the public IP and connect over SSH

Oracle notes that an instance launch automatically attaches a primary VNIC and that IP addresses can be retrieved by getting the VNIC ID and then calling the private IP operation ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

```bash
VNIC_ID=$(oci compute vnic-attachment list \
  --compartment-id "$COMPARTMENT_ID" \
  --instance-id "$INSTANCE_ID" \
  --query 'data[0]."vnic-id"' \
  --raw-output)

PUBLIC_IP=$(oci network vnic get \
  --vnic-id "$VNIC_ID" \
  --query 'data."public-ip"' \
  --raw-output)

echo "$PUBLIC_IP"
ssh -i ~/.ssh/oci_demo_ed25519 opc@"$PUBLIC_IP"
```

The default username depends on the image, and Oracle Linux platform images commonly use `opc`.

### 2.5 Launch with cloud-init user data

The instance launch command supports `--user-data-file`, which Oracle describes as a file containing data that Cloud-Init can use to run custom scripts or provide Cloud-Init configuration ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

```bash
cat > cloud-init.yaml <<'EOF'
#cloud-config
package_update: true
packages:
  - nginx
runcmd:
  - systemctl enable --now nginx
  - echo "hello from OCI CLI" > /usr/share/nginx/html/index.html
EOF

INSTANCE_ID=$(oci compute instance launch \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "cli-nginx-vm" \
  --shape "VM.Standard.E5.Flex" \
  --shape-config '{"ocpus": 1, "memoryInGBs": 8}' \
  --image-id "$IMAGE_ID" \
  --subnet-id "$SUBNET_ID" \
  --assign-public-ip true \
  --ssh-authorized-keys-file ~/.ssh/oci_demo_ed25519.pub \
  --user-data-file cloud-init.yaml \
  --wait-for-state RUNNING \
  --query 'data.id' \
  --raw-output)
```

Do not set both `--user-data-file` and a `user_data` key inside `--metadata` in the same call, because Oracle documents `--user-data-file` as a convenience wrapper around the `user_data` metadata field and says populating both values results in an error ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

### 2.6 Use metadata directly

The launch command supports `--metadata` for custom key-value pairs such as SSH public keys, and the `ssh_authorized_keys` field can be supplied through metadata instead of `--ssh-authorized-keys-file` ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

```bash
SSH_PUBLIC_KEY=$(cat ~/.ssh/oci_demo_ed25519.pub)

oci compute instance launch \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "cli-metadata-vm" \
  --shape "VM.Standard.E5.Flex" \
  --shape-config '{"ocpus": 1, "memoryInGBs": 8}' \
  --image-id "$IMAGE_ID" \
  --subnet-id "$SUBNET_ID" \
  --metadata "{\"ssh_authorized_keys\": \"$SSH_PUBLIC_KEY\"}" \
  --wait-for-state RUNNING
```

### 2.7 Lifecycle actions: start, stop, reset, terminate

Oracle documents `oci compute instance action` for power actions including `START`, `STOP`, `RESET`, `SOFTSTOP`, and `SOFTRESET` ([instance action reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/action.html)).

```bash
# Gracefully stop.
oci compute instance action \
  --instance-id "$INSTANCE_ID" \
  --action SOFTSTOP \
  --wait-for-state STOPPED

# Start again.
oci compute instance action \
  --instance-id "$INSTANCE_ID" \
  --action START \
  --wait-for-state RUNNING

# Reboot now.
oci compute instance action \
  --instance-id "$INSTANCE_ID" \
  --action RESET \
  --wait-for-state RUNNING
```

Oracle documents `oci compute instance terminate` as permanently terminating an instance, automatically detaching attached VNICs and volumes, and optionally preserving or deleting boot and data volumes ([instance terminate reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/terminate.html)).

```bash
# Preserve the boot volume.
oci compute instance terminate \
  --instance-id "$INSTANCE_ID" \
  --preserve-boot-volume true \
  --force \
  --wait-for-state TERMINATED

# Delete the boot volume.
oci compute instance terminate \
  --instance-id "$INSTANCE_ID" \
  --preserve-boot-volume false \
  --force \
  --wait-for-state TERMINATED
```

### 2.8 Preemptible instances for interruptible workloads

Preemptible instances behave like regular compute instances, but Oracle states that capacity can be reclaimed when needed elsewhere and the instances are terminated ([Preemptible Instances](https://docs.oracle.com/iaas/Content/Compute/Concepts/preemptible.htm)). Oracle also states that preemptible instances are suitable for fault-tolerant workloads and not suitable for long-running workloads because they can be terminated at any time ([Preemptible Instances](https://docs.oracle.com/iaas/Content/Compute/Concepts/preemptible.htm)).

```bash
cat > preemptible-config.json <<'EOF'
{
  "preemptionAction": {
    "type": "TERMINATE",
    "preserveBootVolume": false
  }
}
EOF

PREEMPTIBLE_ID=$(oci compute instance launch \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "cli-preemptible-worker" \
  --shape "VM.Standard.E5.Flex" \
  --shape-config '{"ocpus": 1, "memoryInGBs": 8}' \
  --image-id "$IMAGE_ID" \
  --subnet-id "$SUBNET_ID" \
  --ssh-authorized-keys-file ~/.ssh/oci_demo_ed25519.pub \
  --preemptible-instance-config file://preemptible-config.json \
  --wait-for-state RUNNING \
  --query 'data.id' \
  --raw-output)
```

The launch command exposes `--preemptible-instance-config` as a complex JSON parameter and recommends generating example JSON for complex parameters with `--generate-param-json-input` when needed ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)).

```bash
oci compute instance launch --generate-param-json-input preemptible-instance-config
```

### 2.9 Bare metal versus VM shapes

OCI offers both bare metal and VM instances; Oracle describes bare metal as dedicated physical server access for highest performance and strong isolation, while VMs are suitable for applications that do not need the full physical machine ([Compute overview](https://docs.oracle.com/iaas/Content/Compute/Concepts/computeoverview.htm)).

```bash
# List VM shapes.
oci compute shape list \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --query 'data[?starts_with(shape, `VM.`)].shape' \
  --output table

# List bare metal shapes.
oci compute shape list \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --query 'data[?starts_with(shape, `BM.`)].shape' \
  --output table
```

Use VM shapes for most web services, APIs, dev/test, and horizontally scalable workloads, and use bare metal shapes when the workload needs dedicated hardware, full-host licensing, local NVMe, high-end GPU, or RDMA cluster networking.

### 2.10 Instance pools and autoscaling

An instance configuration is a template that defines settings used when creating Compute instances, and Oracle documents `oci compute-management instance-configuration create` with required `--compartment-id` and `--instance-details` ([instance-configuration create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute-management/instance-configuration/create.html)).

```bash
SSH_PUBLIC_KEY=$(cat ~/.ssh/oci_demo_ed25519.pub)

cat > instance-details.json <<EOF
{
  "instanceType": "compute",
  "launchDetails": {
    "compartmentId": "$COMPARTMENT_ID",
    "availabilityDomain": "$AD",
    "displayName": "pool-member",
    "shape": "VM.Standard.E5.Flex",
    "shapeConfig": {"ocpus": 1, "memoryInGBs": 8},
    "sourceDetails": {"sourceType": "image", "imageId": "$IMAGE_ID"},
    "createVnicDetails": {"subnetId": "$SUBNET_ID", "assignPublicIp": true},
    "metadata": {"ssh_authorized_keys": "$SSH_PUBLIC_KEY"}
  }
}
EOF

INSTANCE_CONFIG_ID=$(oci compute-management instance-configuration create \
  --compartment-id "$COMPARTMENT_ID" \
  --display-name "cli-demo-instance-config" \
  --instance-details file://instance-details.json \
  --query 'data.id' \
  --raw-output)
```

An instance pool is created from an instance configuration, and Oracle documents required pool parameters `--compartment-id`, `--instance-configuration-id`, `--placement-configurations`, and `--size` ([instance-pool create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute-management/instance-pool/create.html)).

```bash
cat > placement-configurations.json <<EOF
[
  {
    "availabilityDomain": "$AD",
    "primarySubnetId": "$SUBNET_ID"
  }
]
EOF

POOL_ID=$(oci compute-management instance-pool create \
  --compartment-id "$COMPARTMENT_ID" \
  --display-name "cli-demo-pool" \
  --instance-configuration-id "$INSTANCE_CONFIG_ID" \
  --placement-configurations file://placement-configurations.json \
  --size 2 \
  --wait-for-state RUNNING \
  --query 'data.id' \
  --raw-output)
```

Autoscaling configurations require a resource JSON and policies JSON, and Oracle documents `oci autoscaling configuration create` with required `--compartment-id`, `--policies`, and `--resource` ([autoscaling configuration create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/autoscaling/configuration/create.html)).

```bash
cat > autoscaling-resource.json <<EOF
{
  "id": "$POOL_ID",
  "type": "instancePool"
}
EOF

cat > autoscaling-policies.json <<'EOF'
[
  {
    "displayName": "cpu-autoscale",
    "policyType": "threshold",
    "capacity": {"initial": 2, "min": 1, "max": 5},
    "rules": [
      {
        "displayName": "scale-out-high-cpu",
        "metric": {
          "metricType": "CPU_UTILIZATION",
          "threshold": {"operator": "GT", "value": 70}
        },
        "action": {"type": "CHANGE_COUNT_BY", "value": 1}
      },
      {
        "displayName": "scale-in-low-cpu",
        "metric": {
          "metricType": "CPU_UTILIZATION",
          "threshold": {"operator": "LT", "value": 25}
        },
        "action": {"type": "CHANGE_COUNT_BY", "value": -1}
      }
    ]
  }
]
EOF

AUTOSCALING_CONFIG_ID=$(oci autoscaling configuration create \
  --compartment-id "$COMPARTMENT_ID" \
  --display-name "cli-demo-pool-autoscaling" \
  --resource file://autoscaling-resource.json \
  --policies file://autoscaling-policies.json \
  --cool-down-in-seconds 300 \
  --is-enabled true \
  --query 'data.id' \
  --raw-output)
```

For complex JSON parameters, use each command's `--generate-param-json-input` or `--generate-full-command-json-input` because the OCI CLI command reference documents these helpers for generating JSON templates ([OCI CLI command reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/oci.html)).

```bash
oci compute-management instance-configuration create --generate-param-json-input instance-details
oci compute-management instance-pool create --generate-param-json-input placement-configurations
oci autoscaling configuration create --generate-param-json-input policies
oci autoscaling configuration create --generate-param-json-input resource
```

---

## 3. Containers: OCI Container Instances

OCI Container Instances run containerized workloads without creating a full VM or Kubernetes cluster, and the CLI command `oci container-instances container-instance create` creates a container instance and deploys containers on it ([container instance create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/container-instances/container-instance/create.html)).

Good fits include one-off jobs, batch workers, scheduled tasks, small APIs, and utility containers where Kubernetes would be excessive.

### 3.1 Create a simple container instance

The container instance create command requires `--availability-domain`, `--compartment-id`, `--containers`, `--shape`, `--shape-config`, and `--vnics` ([container instance create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/container-instances/container-instance/create.html)).

```bash
cat > ci-containers.json <<'EOF'
[
  {
    "displayName": "hello",
    "imageUrl": "docker.io/library/nginx:latest",
    "resourceConfig": {
      "vcpusLimit": 1,
      "memoryLimitInGBs": 1
    },
    "ports": [
      {"port": 80, "protocol": "TCP"}
    ]
  }
]
EOF

cat > ci-vnics.json <<EOF
[
  {
    "displayName": "ci-vnic",
    "subnetId": "$SUBNET_ID",
    "isPublicIpAssigned": true
  }
]
EOF

CONTAINER_INSTANCE_ID=$(oci container-instances container-instance create \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "cli-nginx-container" \
  --shape "CI.Standard.E4.Flex" \
  --shape-config '{"ocpus": 1, "memoryInGBs": 2}' \
  --containers file://ci-containers.json \
  --vnics file://ci-vnics.json \
  --container-restart-policy ALWAYS \
  --query 'data.id' \
  --raw-output)

echo "$CONTAINER_INSTANCE_ID"
```

The CLI reference states that `--containers`, `--shape-config`, and `--vnics` are complex JSON values that can be provided as inline JSON strings or as files with `file://` syntax ([container instance create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/container-instances/container-instance/create.html)).

### 3.2 Generate JSON templates

Use generated templates whenever the schema is unclear, because the command reference explicitly recommends generating example JSON for complex parameters ([container instance create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/container-instances/container-instance/create.html)).

```bash
oci container-instances container-instance create --generate-param-json-input containers > containers.template.json
oci container-instances container-instance create --generate-param-json-input vnics > vnics.template.json
oci container-instances container-instance create --generate-param-json-input shape-config > shape-config.template.json
```

### 3.3 Inspect and read logs

List containers belonging to the container instance, then fetch recent logs from a container.

```bash
oci container-instances container list \
  --compartment-id "$COMPARTMENT_ID" \
  --container-instance-id "$CONTAINER_INSTANCE_ID" \
  --output table

CONTAINER_ID=$(oci container-instances container list \
  --compartment-id "$COMPARTMENT_ID" \
  --container-instance-id "$CONTAINER_INSTANCE_ID" \
  --query 'data[0].id' \
  --raw-output)

oci container-instances container retrieve-logs \
  --container-id "$CONTAINER_ID" \
  --file -
```

Oracle documents `oci container-instances container retrieve-logs` as returning the most recent 256 KB of logs from the specified container, and the command requires `--container-id` and `--file` ([container retrieve-logs reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/container-instances/container/retrieve-logs.html)).

### 3.4 Clean up container instances

```bash
oci container-instances container-instance delete \
  --container-instance-id "$CONTAINER_INSTANCE_ID" \
  --force
```

---

## 4. Kubernetes: Container Engine for Kubernetes (OKE)

OKE cluster creation uses the Container Engine CLI group `oci ce`, and Oracle documents `oci ce cluster create` with required `--compartment-id`, `--kubernetes-version`, `--name`, and `--vcn-id` ([OKE cluster create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/ce/cluster/create.html)).

### 4.1 Create a cluster

```bash
export K8S_VERSION="v1.31.1"

CLUSTER_ID=$(oci ce cluster create \
  --compartment-id "$COMPARTMENT_ID" \
  --name "cli-demo-oke" \
  --kubernetes-version "$K8S_VERSION" \
  --vcn-id "$VCN_ID" \
  --type ENHANCED_CLUSTER \
  --service-lb-subnet-ids "[\"$SUBNET_ID\"]" \
  --endpoint-subnet-id "$SUBNET_ID" \
  --cluster-pod-network-options '[{"cniType":"FLANNEL_OVERLAY"}]' \
  --wait-for-state SUCCEEDED \
  --query 'data.resources[0].identifier' \
  --raw-output)

echo "$CLUSTER_ID"
```

The `--endpoint-subnet-id` option places the cluster endpoint in a regional subnet, and `--service-lb-subnet-ids` identifies subnets configured to host Kubernetes load balancers ([OKE cluster create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/ce/cluster/create.html)). If your OKE command returns a work request rather than a cluster OCID, list clusters after the work request succeeds:

```bash
CLUSTER_ID=$(oci ce cluster list \
  --compartment-id "$COMPARTMENT_ID" \
  --name "cli-demo-oke" \
  --query 'data[0].id' \
  --raw-output)
```

### 4.2 Create a node pool

The node pool create command requires `--cluster-id`, `--compartment-id`, `--name`, and `--node-shape`, while options such as `--kubernetes-version`, `--node-image-id`, `--node-shape-config`, `--placement-configs`, `--size`, and `--ssh-public-key` configure the pool ([OKE node-pool create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/ce/node-pool/create.html)).

```bash
OKE_IMAGE_ID=$(oci ce node-pool-options get \
  --node-pool-option-id all \
  --compartment-id "$COMPARTMENT_ID" \
  --query "data.sources[?contains(\"source-name\", 'OKE')].\"image-id\" | [0]" \
  --raw-output)

oci ce node-pool create \
  --compartment-id "$COMPARTMENT_ID" \
  --cluster-id "$CLUSTER_ID" \
  --name "cli-demo-pool" \
  --kubernetes-version "$K8S_VERSION" \
  --node-shape "VM.Standard.E5.Flex" \
  --node-shape-config '{"ocpus": 2, "memoryInGBs": 16}' \
  --node-image-id "$OKE_IMAGE_ID" \
  --node-boot-volume-size-in-gbs 100 \
  --size 2 \
  --placement-configs "[{\"availabilityDomain\": \"$AD\", \"subnetId\": \"$SUBNET_ID\"}]" \
  --ssh-public-key "$(cat ~/.ssh/oci_demo_ed25519.pub)" \
  --wait-for-state SUCCEEDED
```

Oracle's GPU OKE tutorial uses `oci ce node-pool-options get` to find an OKE-compatible node image and `oci ce node-pool create` with `--placement-configs` to create a GPU node pool ([OKE GPU tutorial](https://docs.oracle.com/en/learn/deploy-vllm-production-stack-oke/index.html)).

### 4.3 Configure `kubectl`

Oracle documents `oci ce cluster create-kubeconfig` for creating the kubeconfig YAML and notes that the default output file is `~/.kube/config` ([create-kubeconfig reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/ce/cluster/create-kubeconfig.html)).

```bash
mkdir -p ~/.kube
oci ce cluster create-kubeconfig \
  --cluster-id "$CLUSTER_ID" \
  --file ~/.kube/config \
  --region "$REGION" \
  --token-version 2.0.0 \
  --kube-endpoint PUBLIC_ENDPOINT \
  --overwrite

kubectl get nodes
```

Use `--kube-endpoint PRIVATE_ENDPOINT` for a private endpoint, and Oracle documents `PUBLIC_ENDPOINT`, `PRIVATE_ENDPOINT`, `VCN_HOSTNAME`, and `LEGACY_KUBERNETES` as accepted kube endpoint values ([create-kubeconfig reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/ce/cluster/create-kubeconfig.html)).

### 4.4 Deploy a workload

```bash
kubectl create deployment hello \
  --image=nginx:latest \
  --replicas=2

kubectl expose deployment hello \
  --type=LoadBalancer \
  --port=80 \
  --target-port=80

kubectl get pods -o wide
kubectl get service hello
```

OKE workloads are deployed with Kubernetes tools after kubeconfig is created, while OCI CLI remains useful for cluster, node pool, network, IAM, and lifecycle operations.

### 4.5 OKE cleanup

```bash
NODE_POOL_ID=$(oci ce node-pool list \
  --compartment-id "$COMPARTMENT_ID" \
  --cluster-id "$CLUSTER_ID" \
  --query 'data[0].id' \
  --raw-output)

oci ce node-pool delete --node-pool-id "$NODE_POOL_ID" --force
oci ce cluster delete --cluster-id "$CLUSTER_ID" --force
```

---

## 5. Functions: serverless workloads

OCI Functions is based on the Fn Project workflow, and Oracle's Functions CLI tutorial states that the Fn client is used to create, manage, and deploy Oracle Functions ([Functions CLI setup tutorial](https://docs.oracle.com/en-us/iaas/Content/developer/functions/func-setup-cli/01-summary.htm)). The OCI CLI can manage applications and function definitions, while typical source-to-deploy workflows use the `fn` CLI.

### 5.1 Create an application with `oci fn`

`oci fn application create` creates a new application and requires `--compartment-id`, `--display-name`, and `--subnet-ids` ([fn application create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/fn/application/create.html)).

```bash
APP_ID=$(oci fn application create \
  --compartment-id "$COMPARTMENT_ID" \
  --display-name "cli-demo-functions-app" \
  --subnet-ids "[\"$SUBNET_ID\"]" \
  --config '{"ENV":"demo"}' \
  --query 'data.id' \
  --raw-output)
```

### 5.2 Create a function definition from an existing image

`oci fn function create` creates a function and requires `--application-id`, `--display-name`, and `--memory-in-mbs` ([fn function create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/fn/function/create.html)). Oracle's Functions documentation notes that a function can be created from an existing Docker image with the OCI CLI, while `fn deploy` is the common one-step workflow for building, pushing, and creating a function ([Creating functions from existing images](https://docs.oracle.com/en-us/iaas/Content/Functions/Tasks/functionscreatingfunctions.htm)).

```bash
oci fn function create \
  --application-id "$APP_ID" \
  --display-name "hello-func" \
  --memory-in-mbs 256 \
  --image "iad.ocir.io/mytenancyns/functions/hello-func:0.0.1" \
  --timeout-in-seconds 30
```

### 5.3 Deploy with `fn`

Oracle's Functions tutorial documents installing the Fn client, creating an Oracle provider context, setting `oracle.compartment-id`, setting the Functions API URL, setting the OCIR registry URL, and deploying with `fn -v deploy --app <app-name>` ([Functions CLI setup tutorial](https://docs.oracle.com/en-us/iaas/Content/developer/functions/func-setup-cli/01-summary.htm)).

```bash
# Install fn on Linux/macOS/WSL.
curl -LSs https://raw.githubusercontent.com/fnproject/cli/master/install | sh
fn version

# Configure Fn for OCI.
fn create context "$REGION" --provider oracle
fn use context "$REGION"
fn update context oracle.compartment-id "$COMPARTMENT_ID"
fn update context api-url "https://functions.${REGION}.oci.oraclecloud.com"
fn update context registry "iad.ocir.io/mytenancyns/functions"

# Create and deploy a Python function.
fn init --runtime python hello-func
cd hello-func
fn -v deploy --app cli-demo-functions-app
fn invoke cli-demo-functions-app hello-func
```

Oracle's tutorial also shows invoking a function with `fn invoke`, inspecting the invoke endpoint, and using `oci raw-request` to send a signed request to a function endpoint ([Functions CLI setup tutorial](https://docs.oracle.com/en-us/iaas/Content/developer/functions/func-setup-cli/01-summary.htm)).

```bash
fn inspect function cli-demo-functions-app hello-func
oci raw-request \
  --http-method POST \
  --request-body "" \
  --target-uri "https://<function-endpoint>/actions/invoke"
```

---

## 6. GPU and HPC workloads

OCI GPU shapes are designed for hardware-accelerated workloads, and Oracle's shape reference lists GPU families such as `BM.GPU.A10`, `BM.GPU.A100`, `BM.GPU.H100.8`, `BM.GPU.H200.8`, and `BM.GPU.B200.8` ([Compute shapes reference](https://docs.oracle.com/en-us/iaas/Content/Compute/References/computeshapes.htm)). Oracle's shape reference also states that some bare metal GPU shapes support cluster networking ([Compute shapes reference](https://docs.oracle.com/en-us/iaas/Content/Compute/References/computeshapes.htm)).

### 6.1 Discover GPU shapes

```bash
oci compute shape list \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --all \
  --query 'data[?contains(shape, `GPU`)].{shape:shape, ocpus:ocpus, memory:"memory-in-gbs", gpus:gpus}' \
  --output table
```

Oracle's shape reference states that `BM.GPU.A10` uses NVIDIA A10 24 GB GPUs, `BM.GPU.A100` uses NVIDIA A100 80 GB GPUs, and `BM.GPU.H100.8` uses 8 NVIDIA H100 80 GB GPUs ([Compute shapes reference](https://docs.oracle.com/en-us/iaas/Content/Compute/References/computeshapes.htm)).

### 6.2 Launch a GPU VM or bare metal instance

```bash
GPU_SHAPE="VM.GPU.A10.1"

GPU_INSTANCE_ID=$(oci compute instance launch \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "cli-gpu-worker" \
  --shape "$GPU_SHAPE" \
  --image-id "$IMAGE_ID" \
  --subnet-id "$SUBNET_ID" \
  --assign-public-ip true \
  --ssh-authorized-keys-file ~/.ssh/oci_demo_ed25519.pub \
  --boot-volume-size-in-gbs 200 \
  --wait-for-state RUNNING \
  --query 'data.id' \
  --raw-output)
```

Use larger boot volumes for GPU images and ML frameworks, validate driver support for the selected image, and check limits before launch because GPU capacity is region- and availability-domain-specific.

### 6.3 GPU node pools on OKE

Oracle's vLLM OKE tutorial shows creating GPU node pools with `oci ce node-pool create`, selecting GPU shapes such as `VM.GPU.A10.1`, `BM.GPU4.8`, `BM.GPU.A100-v2.8`, and `BM.GPU.H100.8`, and looking up OKE-compatible GPU node images through `oci ce node-pool-options get` ([OKE GPU tutorial](https://docs.oracle.com/en/learn/deploy-vllm-production-stack-oke/index.html)).

```bash
GPU_SHAPE="VM.GPU.A10.1"
GPU_NODE_COUNT=1
GPU_BOOT_VOLUME_GB=200

GPU_IMAGE_ID=$(oci ce node-pool-options get \
  --node-pool-option-id all \
  --compartment-id "$COMPARTMENT_ID" \
  --query "data.sources[?contains(\"source-name\", 'GPU') && contains(\"source-name\", 'OKE')].\"image-id\" | [0]" \
  --raw-output)

oci ce node-pool create \
  --compartment-id "$COMPARTMENT_ID" \
  --cluster-id "$CLUSTER_ID" \
  --name "gpu-pool" \
  --kubernetes-version "$K8S_VERSION" \
  --node-shape "$GPU_SHAPE" \
  --node-image-id "$GPU_IMAGE_ID" \
  --node-boot-volume-size-in-gbs "$GPU_BOOT_VOLUME_GB" \
  --size "$GPU_NODE_COUNT" \
  --placement-configs "[{\"availabilityDomain\": \"$AD\", \"subnetId\": \"$SUBNET_ID\"}]" \
  --initial-node-labels '[{"key":"workload","value":"gpu"},{"key":"nvidia.com/gpu","value":"true"}]'
```

Oracle's GPU tutorial notes that GPU capacity varies by region and availability domain and recommends trying a different availability domain or GPU shape if node pool creation fails with an out-of-host-capacity error ([OKE GPU tutorial](https://docs.oracle.com/en/learn/deploy-vllm-production-stack-oke/index.html)).

### 6.4 HPC, RDMA, compute clusters, and cluster networks

Oracle describes RDMA cluster networks as groups of HPC, GPU, or optimized instances connected with high-bandwidth, ultra-low-latency networking, with latency as low as single-digit microseconds ([High Performance Computing](https://docs.oracle.com/en-us/iaas/Content/Compute/References/high-performance-compute.htm)). Oracle documents two RDMA approaches: cluster networks with instance pools for predictable capacity and identical instances, and compute clusters for individually managed instances in the RDMA network group ([High Performance Computing](https://docs.oracle.com/en-us/iaas/Content/Compute/References/high-performance-compute.htm)).

Create an empty compute cluster, then launch instances into it with `--compute-cluster-id`.

```bash
COMPUTE_CLUSTER_ID=$(oci compute compute-cluster create \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "cli-hpc-compute-cluster" \
  --query 'data.id' \
  --raw-output)

oci compute instance launch \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "hpc-node-1" \
  --shape "BM.HPC.E5.144" \
  --image-id "$IMAGE_ID" \
  --subnet-id "$SUBNET_ID" \
  --ssh-authorized-keys-file ~/.ssh/oci_demo_ed25519.pub \
  --compute-cluster-id "$COMPUTE_CLUSTER_ID" \
  --wait-for-state RUNNING
```

Oracle documents `oci compute compute-cluster create` as creating an empty RDMA network group and states that instances must be created in the same compartment and availability domain as the compute cluster ([compute-cluster create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/compute-cluster/create.html)).

---

## 7. Best practices

### 7.1 Cost management

Stop instances when preserving boot volume state but not needing CPU or memory, and terminate instances when finished; Oracle documents stop/start/reset actions with `oci compute instance action` and permanent deletion with `oci compute instance terminate` ([instance action reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/action.html), [instance terminate reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/terminate.html)). Use preemptible capacity for interruptible workloads because Oracle states preemptible capacity costs less than on-demand capacity but can be reclaimed at any time ([Preemptible Instances](https://docs.oracle.com/iaas/Content/Compute/Concepts/preemptible.htm)).

```bash
# Find running demo instances.
oci compute instance list \
  --compartment-id "$COMPARTMENT_ID" \
  --lifecycle-state RUNNING \
  --query 'data[?contains("display-name", `cli-`)].{name:"display-name",id:id}' \
  --output table
```

### 7.2 Tag everything

OCI CLI resource creation commands commonly support free-form and defined tags, and Oracle's command references show free-form tag JSON such as `{"Department":"Finance"}` and defined tag JSON such as `{"Operations":{"CostCenter":"42"}}` ([instance-pool create reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute-management/instance-pool/create.html)).

```bash
oci compute instance launch \
  --compartment-id "$COMPARTMENT_ID" \
  --availability-domain "$AD" \
  --display-name "tagged-vm" \
  --shape "VM.Standard.E5.Flex" \
  --shape-config '{"ocpus": 1, "memoryInGBs": 8}' \
  --image-id "$IMAGE_ID" \
  --subnet-id "$SUBNET_ID" \
  --ssh-authorized-keys-file ~/.ssh/oci_demo_ed25519.pub \
  --freeform-tags '{"project":"platform","env":"dev"}' \
  --defined-tags '{"Operations":{"CostCenter":"42"}}'
```

### 7.3 IAM and least privilege

IAM policies govern resource access in OCI, and Oracle describes the policy statement syntax as `Allow <subject> to <verb> <resource-type> in <location> where <conditions>` ([Policy Syntax](https://docs.oracle.com/en-us/iaas/Content/Identity/Concepts/policysyntax.htm)). Grant human users, CI systems, and instance principals the smallest resource family and compartment scope needed for the task.

Example policy statements for a group that launches compute in one compartment:

```text
Allow group ComputeOperators to manage instances in compartment Dev
Allow group ComputeOperators to use subnets in compartment Dev
Allow group ComputeOperators to use vnics in compartment Dev
Allow group ComputeOperators to read images in compartment Dev
Allow group ComputeOperators to inspect compartments in tenancy
Allow group ComputeOperators to manage compute-capacity-reports in tenancy
```

Oracle's common policy documentation includes a policy statement for users to determine capacity availability before creating an instance: `Allow group InstanceLaunchers to manage compute-capacity-reports in tenancy` ([Common Policies](https://docs.oracle.com/iaas/Content/Identity/Concepts/commonpolicies.htm)).

Example policy statements for instance principals:

```text
Allow dynamic-group AppInstances to read object-family in compartment AppData
Allow dynamic-group AppInstances to use keys in compartment Security
```

Dynamic groups contain resources such as compute instances that match rules, and Oracle states these instances can act as principal actors and make API calls according to policies written for the dynamic group ([IAM overview](https://docs.oracle.com/iaas/Content/Identity/Concepts/overview.htm)).

### 7.4 Service limits and capacity

OCI tenancies have service limits configured by Oracle, and Oracle states users can request service limit increases while using quotas to control usage distribution across compartments ([IAM security structure](https://docs.oracle.com/en-us/iaas/Content/cloud-adoption-framework/iam-security-structure.htm)). Capacity errors can be shape-, region-, or availability-domain-specific, and Oracle's GPU OKE tutorial recommends trying a different availability domain or shape when encountering out-of-host-capacity errors ([OKE GPU tutorial](https://docs.oracle.com/en/learn/deploy-vllm-production-stack-oke/index.html)).

```bash
# List limits services; then inspect compute limits in the Console or with limits commands.
oci limits service list --all --output table
oci limits value list --service-name compute --all --output table
```

### 7.5 Troubleshooting checklist

| Symptom | Likely cause | CLI checks and fixes |
| --- | --- | --- |
| `NotAuthenticated` or signing error | Bad config path, bad key, expired session token, wrong profile, or public key not uploaded | Run `oci os ns get --debug`; validate `~/.oci/config`; for session profiles, use `--auth security_token` and refresh with `oci session refresh` ([token-based authentication docs](https://docs.oracle.com/en-us/iaas/Content/API/SDKDocs/clitoken.htm)). |
| `NotAuthorizedOrNotFound` | Missing IAM policy or wrong compartment OCID | Confirm `COMPARTMENT_ID`, inspect policies, and check that the principal has permissions in the target compartment; OCI policies are compartment-scoped or tenancy-scoped ([Policy Syntax](https://docs.oracle.com/en-us/iaas/Content/Identity/Concepts/policysyntax.htm)). |
| `LimitExceeded` | Service limit or compartment quota | Check limits and request increases; Oracle notes tenancies have service limits and administrators can set compartment quotas ([IAM security structure](https://docs.oracle.com/en-us/iaas/Content/cloud-adoption-framework/iam-security-structure.htm)). |
| `Out of host capacity` | No capacity for selected shape in selected AD | Try another availability domain, another shape, smaller size, a capacity reservation, or a staged rollout; Oracle's GPU tutorial recommends trying a different AD or GPU shape for out-of-host-capacity failures ([OKE GPU tutorial](https://docs.oracle.com/en/learn/deploy-vllm-production-stack-oke/index.html)). |
| Instance unreachable by SSH | Missing public IP, route table, internet gateway, security rule, NSG rule, wrong user, or wrong key | Confirm VNIC public IP, subnet route, ingress TCP 22, `opc` user for Oracle Linux, and SSH key supplied with `--ssh-authorized-keys-file`. |
| Cloud-init did not run | Invalid user-data format or script error | SSH to the VM and inspect `/var/log/cloud-init.log` and `/var/log/cloud-init-output.log`; ensure `--user-data-file` is not combined with metadata `user_data` ([Compute instance launch reference](https://docs.oracle.com/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/compute/instance/launch.html)). |
| OKE nodes not ready | Wrong node image, subnet, security rules, route, service gateway, or capacity | Use `oci ce node-pool get`, `kubectl describe node`, and `kubectl get pods -n kube-system`; Oracle's GPU tutorial notes OKE nodes may take minutes while provisioning, booting, installing GPU drivers, and registering ([OKE GPU tutorial](https://docs.oracle.com/en/learn/deploy-vllm-production-stack-oke/index.html)). |

### 7.6 Cleanup commands

```bash
# Terminate a single compute instance.
oci compute instance terminate \
  --instance-id "$INSTANCE_ID" \
  --preserve-boot-volume false \
  --force \
  --wait-for-state TERMINATED

# Delete an instance pool after scaling it down.
oci compute-management instance-pool update \
  --instance-pool-id "$POOL_ID" \
  --size 0
oci compute-management instance-pool delete \
  --instance-pool-id "$POOL_ID" \
  --force

# Delete autoscaling configuration.
oci autoscaling configuration delete \
  --auto-scaling-configuration-id "$AUTOSCALING_CONFIG_ID" \
  --force

# Delete a container instance.
oci container-instances container-instance delete \
  --container-instance-id "$CONTAINER_INSTANCE_ID" \
  --force

# Delete OKE resources.
oci ce node-pool delete --node-pool-id "$NODE_POOL_ID" --force
oci ce cluster delete --cluster-id "$CLUSTER_ID" --force
```

---

## 8. Quick command reference

| Task | Command |
| --- | --- |
| Verify CLI auth | `oci os ns get` |
| List regions | `oci iam region list` |
| List availability domains | `oci iam availability-domain list --compartment-id "$COMPARTMENT_ID"` |
| List images | `oci compute image list --compartment-id "$COMPARTMENT_ID" --all` |
| List shapes | `oci compute shape list --compartment-id "$COMPARTMENT_ID" --availability-domain "$AD" --all` |
| Launch VM | `oci compute instance launch --compartment-id ... --availability-domain ... --shape ... --image-id ... --subnet-id ...` |
| Stop/start/reset VM | `oci compute instance action --instance-id ... --action STOP|START|RESET` |
| Terminate VM | `oci compute instance terminate --instance-id ... --force` |
| Create instance config | `oci compute-management instance-configuration create --compartment-id ... --instance-details file://instance-details.json` |
| Create instance pool | `oci compute-management instance-pool create --compartment-id ... --instance-configuration-id ... --placement-configurations file://placement.json --size 2` |
| Create autoscaling config | `oci autoscaling configuration create --compartment-id ... --resource file://resource.json --policies file://policies.json` |
| Create container instance | `oci container-instances container-instance create --compartment-id ... --availability-domain ... --containers file://containers.json --vnics file://vnics.json --shape ... --shape-config ...` |
| Create OKE cluster | `oci ce cluster create --compartment-id ... --kubernetes-version ... --name ... --vcn-id ...` |
| Create OKE node pool | `oci ce node-pool create --compartment-id ... --cluster-id ... --node-shape ... --placement-configs ... --size ...` |
| Get kubeconfig | `oci ce cluster create-kubeconfig --cluster-id ... --file ~/.kube/config --region ... --token-version 2.0.0` |
| Create Functions app | `oci fn application create --compartment-id ... --display-name ... --subnet-ids '["..."]'` |
| Create Functions function | `oci fn function create --application-id ... --display-name ... --memory-in-mbs ... --image ...` |

The key pattern across OCI CLI compute work is discover OCIDs with list commands, capture IDs with `--query` and `--raw-output`, pass complex objects as JSON files, and keep `--compartment-id`, availability domain, and region explicit until your automation is stable.
