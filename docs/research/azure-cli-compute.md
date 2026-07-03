# Research: Azure CLI Compute Guide

> **Provenance.** Captured 2026-06-26 from a user-supplied hands-on guide
> ("Azure CLI Compute Guide: Launch Workloads on Azure with `az`"), whose command
> syntax was checked against Microsoft Learn (Azure CLI getting-started, command
> reference, and compute quickstarts; primary-source links are inline below).
>
> **Status:** reference / background. It is the cited basis for the `az vmss`
> command shapes the **reference Azure provisioner** uses — `AzCliControlPlane`
> (`src/core/azure_cli_control_plane.cpp`); see
> [azure-provisioner.md](../cloud/azure-provisioner.md). zetta's burst backend uses
> only the **VMSS + Spot** slice (§2.6–2.7); the rest (VMs, ACI, AKS, Batch,
> Container Apps, Functions, GPU/HPC) is background for choosing primitives and
> for the AWS/GCP equivalents in
> [cloud-provisioner-pattern.md](../cloud/cloud-provisioner-pattern.md). Examples are
> illustrative — replace names, regions, VM sizes, and images with values your
> subscription allows.

---

# Azure CLI Compute Guide: Launch Workloads on Azure with `az`

This hands-on guide shows practical Azure CLI patterns for launching workloads on Azure Virtual Machines, Virtual Machine Scale Sets, Azure Container Instances, Azure Kubernetes Service, Azure Batch, Azure Container Apps, Azure Functions, and GPU/HPC compute. Command syntax and examples were checked against Microsoft Learn Azure CLI and Azure service documentation, including the Azure CLI getting-started guide, command reference pages, and compute quickstarts ([Azure CLI get started](https://learn.microsoft.com/en-us/cli/azure/get-started-with-azure-cli?view=azure-cli-latest), [Azure CLI install](https://learn.microsoft.com/en-us/cli/azure/install-azure-cli), [az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)).

> Assumptions: examples use Bash syntax, the Azure public cloud, and a new resource group per tutorial run. Replace names, regions, VM sizes, and images with values allowed in your subscription.

## 0. Quick decision guide

| Workload need | Good first Azure compute option | Why |
|---|---|---|
| Full OS control, SSH/RDP, custom daemons | Azure Virtual Machines | Direct VM control with VM sizes, images, disks, identities, and extensions ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)). |
| Horizontally scaled VM workers | VM Scale Sets | Group and scale multiple VMs with shared configuration ([az vmss](https://learn.microsoft.com/en-us/cli/azure/vmss?view=azure-cli-latest)). |
| Run a container quickly without Kubernetes | Azure Container Instances | Create a container group directly and inspect logs with `az container logs` ([az container](https://learn.microsoft.com/en-us/cli/azure/container?view=azure-cli-latest)). |
| Production Kubernetes | Azure Kubernetes Service | Managed Kubernetes cluster plus `kubectl` deployment workflow ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)). |
| Many parallel jobs / HPC / embarrassingly parallel tasks | Azure Batch | Batch accounts, pools, jobs, and tasks are built for scheduled parallel compute ([Azure Batch CLI quickstart](https://learn.microsoft.com/en-us/azure/batch/quick-create-cli)). |
| Serverless containers with scale-to-zero style operations | Azure Container Apps | Deploy existing images or source with `az containerapp up` or `az containerapp create` ([Container Apps up](https://learn.microsoft.com/en-us/azure/container-apps/containerapp-up)). |
| Event-driven serverless code | Azure Functions | Provision a function app with `az functionapp create` and publish with Functions Core Tools ([Azure Functions CLI quickstart](https://learn.microsoft.com/en-us/azure/azure-functions/create-first-function-cli-python)). |

---

## 1. Getting started with Azure CLI

### 1.1 Install or use Cloud Shell

Azure CLI can be installed on Windows, Linux, and macOS, and it can also run in Docker or Azure Cloud Shell ([Azure CLI install](https://learn.microsoft.com/en-us/cli/azure/install-azure-cli)). Azure Cloud Shell already includes Azure CLI, while a local install can be checked with `az version` ([Azure CLI get started](https://learn.microsoft.com/en-us/cli/azure/get-started-with-azure-cli?view=azure-cli-latest)).

```bash
az version
```

Common installation shortcuts:

```bash
# macOS with Homebrew
brew update && brew install azure-cli

# Ubuntu/Debian packages are documented by Microsoft; use the official install page for the current command set.
# Windows users can use WinGet or MSI from the official install page.
```

### 1.2 Sign in and select a subscription

Use `az login` for interactive sign-in, or use `az login --use-device-code` when a browser cannot be opened from the terminal ([Azure CLI get started](https://learn.microsoft.com/en-us/cli/azure/get-started-with-azure-cli?view=azure-cli-latest)). After sign-in, set the active subscription with `az account set --subscription` ([az account](https://learn.microsoft.com/en-us/cli/azure/account?view=azure-cli-latest)).

```bash
az login

# If browser auth is awkward, use device code flow.
az login --use-device-code

# See subscriptions available to your account.
az account list --output table

# Set the active subscription by name or ID.
az account set --subscription "<subscription-id-or-name>"

# Confirm the active subscription.
az account show --output table
```

### 1.3 Choose a region and create a resource group

Azure resource groups are logical containers for resources, and `az group create` requires a name and location ([az group](https://learn.microsoft.com/en-us/cli/azure/group?view=azure-cli-latest)). You can list Azure locations with `az account list-locations`, and the `az group` docs note that location values come from that command ([az group](https://learn.microsoft.com/en-us/cli/azure/group?view=azure-cli-latest)).

```bash
# List commonly used location names.
az account list-locations \
  --query "[].{Name:name, DisplayName:displayName}" \
  --output table

# Set reusable variables.
export LOCATION="westus2"
export RG="rg-compute-demo"

# Create a resource group.
az group create \
  --name "$RG" \
  --location "$LOCATION" \
  --tags project=az-compute-guide environment=dev owner=$(whoami)
```

Key flags:

- `--name` / `-n`: resource group name ([az group](https://learn.microsoft.com/en-us/cli/azure/group?view=azure-cli-latest)).
- `--location` / `-l`: Azure region such as `westus2`, `eastus`, or `canadacentral` ([az group](https://learn.microsoft.com/en-us/cli/azure/group?view=azure-cli-latest)).
- `--tags`: applies metadata tags at creation time; Azure CLI also supports `az tag create` and `az tag update` for tag operations after creation ([Azure tag CLI docs](https://learn.microsoft.com/en-us/azure/azure-resource-manager/management/tag-resources-cli)).

Optional defaults reduce repeated flags:

```bash
az configure --defaults group="$RG" location="$LOCATION"
```

---

## 2. Azure Virtual Machines

Azure Virtual Machines are the most direct `az` compute primitive: you choose an image, size, OS credentials, networking, disks, identity, and lifecycle actions ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)).

### 2.1 Find VM sizes and images

The `az vm list-sizes` command lists available VM sizes for a location, although Microsoft marks it as deprecated in favor of newer SKU lookup patterns such as `az vm list-skus` ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)). The `az vm image list` command lists VM/VMSS Marketplace images and supports `--all`, `--publisher`, `--offer`, `--sku`, and `--location` filters ([az vm image](https://learn.microsoft.com/en-us/cli/azure/vm/image?view=azure-cli-latest)).

```bash
# List VM sizes available in a region.
az vm list-sizes \
  --location "$LOCATION" \
  --output table

# Recommended SKU search pattern for a VM family, including GPU/HPC families.
az vm list-skus \
  --location "$LOCATION" \
  --resource-type virtualMachines \
  --size Standard_D \
  --all \
  --output table

# List cached image aliases.
az vm image list --output table

# Search live Marketplace images.
az vm image list \
  --publisher Canonical \
  --offer 0001-com-ubuntu-server-jammy \
  --all \
  --output table

# Get a specific image URN-like value.
az vm image list \
  --publisher Canonical \
  --offer 0001-com-ubuntu-server-jammy \
  --sku 22_04-lts-gen2 \
  --all \
  --query "[-1].urn" \
  --output tsv
```

### 2.2 Create a Linux VM with SSH keys

The Linux VM quickstart shows `az vm create` with `--resource-group`, `--name`, `--image`, `--admin-username`, `--generate-ssh-keys`, and `--public-ip-sku Standard` ([Linux VM quickstart](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/quick-create-cli)). The `--generate-ssh-keys` option uses an existing key in `~/.ssh` when one exists or generates a new key when needed ([Linux VM quickstart](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/quick-create-cli)).

```bash
export VM_NAME="vm-linux-demo"
export ADMIN_USER="azureuser"
export IMAGE="Ubuntu2204"          # Alias accepted by az vm create.
export SIZE="Standard_B2s"         # Change for production workloads.

az vm create \
  --resource-group "$RG" \
  --name "$VM_NAME" \
  --image "$IMAGE" \
  --size "$SIZE" \
  --admin-username "$ADMIN_USER" \
  --generate-ssh-keys \
  --public-ip-sku Standard \
  --tags workload=web environment=dev
```

Key flags:

- `--image`: image alias, Marketplace URN, custom image name/ID, shared gallery image, or VHD URI; the `az vmss` reference describes the same image value pattern for VMSS images ([az vmss](https://learn.microsoft.com/en-us/cli/azure/vmss?view=azure-cli-latest)).
- `--size`: VM SKU such as `Standard_B2s`, `Standard_D4s_v5`, or an N-series GPU size.
- `--admin-username`: local administrator username.
- `--generate-ssh-keys`: create or reuse SSH keys for Linux authentication ([Linux VM quickstart](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/quick-create-cli)).
- `--assign-identity`: adds a managed identity, useful when the VM accesses Azure resources without storing secrets; the Linux quickstart includes `--assign-identity` in its example ([Linux VM quickstart](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/quick-create-cli)).

### 2.3 Connect over SSH

The Linux VM quickstart stores the public IP from `az vm show --show-details` and then connects with SSH ([Linux VM quickstart](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/quick-create-cli)).

```bash
export VM_IP=$(az vm show \
  --resource-group "$RG" \
  --name "$VM_NAME" \
  --show-details \
  --query publicIps \
  --output tsv)

ssh "$ADMIN_USER@$VM_IP"
```

If SSH fails, check the public IP, network security group rules, username, and key path:

```bash
az vm show -g "$RG" -n "$VM_NAME" --show-details --output table
az vm list-ip-addresses -g "$RG" -n "$VM_NAME" --output table
```

### 2.4 Run a command on a VM without SSH

`az vm run-command invoke` executes a run command on a VM, and the VM command reference lists it under `az vm run-command` ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)). Microsoft’s Windows VM quickstart shows the same pattern with `--command-id RunPowerShellScript` and `--scripts`; Linux VMs commonly use `RunShellScript` ([Windows VM quickstart](https://learn.microsoft.com/en-us/azure/virtual-machines/windows/quick-create-cli), [az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)).

```bash
# Linux: run a shell command inside the VM.
az vm run-command invoke \
  --resource-group "$RG" \
  --name "$VM_NAME" \
  --command-id RunShellScript \
  --scripts "uname -a && df -h && cloud-init status --long"

# Windows example: run PowerShell inside a Windows VM.
az vm run-command invoke \
  --resource-group "$RG" \
  --name "vm-windows-demo" \
  --command-id RunPowerShellScript \
  --scripts "Get-ComputerInfo | Select-Object OsName, OsVersion"
```

### 2.5 Start, stop, and deallocate

`az vm start` starts a stopped VM, `az vm stop` powers off a running VM, and `az vm deallocate` releases compute allocation so charges for compute resources no longer apply and the VM state becomes `Stopped (Deallocated)` ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)). For cost control, use `deallocate` rather than only `stop` when the VM does not need allocated compute capacity ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)).

```bash
# Power off the guest but keep allocation semantics as defined by Azure.
az vm stop -g "$RG" -n "$VM_NAME"

# Release compute allocation to stop compute charges.
az vm deallocate -g "$RG" -n "$VM_NAME"

# Start it again.
az vm start -g "$RG" -n "$VM_NAME"

# Show current power state.
az vm get-instance-view \
  -g "$RG" \
  -n "$VM_NAME" \
  --query "instanceView.statuses[?starts_with(code, 'PowerState/')].displayStatus" \
  -o tsv
```

### 2.6 Create a Spot VM for interruptible workloads

Azure Spot Virtual Machines use unused Azure capacity at lower cost, but Azure can evict them when capacity is needed back, so they fit interruption-tolerant workloads such as batch processing, dev/test, and large compute jobs ([Azure Spot VM CLI](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/spot-cli)). To create one with CLI, add `--priority Spot`, set `--eviction-policy` to `Deallocate` or `Delete`, and set `--max-price` to a price or `-1` ([Azure Spot VM CLI](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/spot-cli)).

```bash
export SPOT_VM="vm-spot-worker"

az vm create \
  --resource-group "$RG" \
  --name "$SPOT_VM" \
  --image Ubuntu2204 \
  --size Standard_D2s_v5 \
  --admin-username "$ADMIN_USER" \
  --generate-ssh-keys \
  --priority Spot \
  --max-price -1 \
  --eviction-policy Deallocate \
  --public-ip-sku Standard \
  --tags workload=batch priority=spot

# Test application behavior on eviction.
az vm simulate-eviction \
  --resource-group "$RG" \
  --name "$SPOT_VM"
```

### 2.7 VM Scale Sets for scaled VM workloads

`az vmss create` creates a Virtual Machine Scale Set, and Microsoft’s VMSS tutorial shows a Flexible orchestration example with `--image Ubuntu2204`, `--orchestration-mode Flexible`, `--admin-username`, and `--generate-ssh-keys` ([VMSS app install tutorial](https://learn.microsoft.com/en-us/azure/virtual-machine-scale-sets/tutorial-install-apps-cli)). The VMSS command reference includes `--instance-count`, and examples show `az vmss create` with an instance count and image ([az vmss](https://learn.microsoft.com/en-us/cli/azure/vmss?view=azure-cli-latest)).

```bash
export VMSS_NAME="vmss-workers"

az vmss create \
  --resource-group "$RG" \
  --name "$VMSS_NAME" \
  --image Ubuntu2204 \
  --orchestration-mode Flexible \
  --instance-count 2 \
  --vm-sku Standard_B2s \
  --admin-username "$ADMIN_USER" \
  --generate-ssh-keys \
  --tags workload=worker environment=dev

# Scale out or in.
az vmss scale \
  --resource-group "$RG" \
  --name "$VMSS_NAME" \
  --new-capacity 5

# Check instance status.
az vmss list-instances \
  --resource-group "$RG" \
  --name "$VMSS_NAME" \
  --output table
```

To use Spot capacity for a scale set, Microsoft’s Spot VMSS doc adds `--priority Spot`, `--eviction-policy`, `--max-price`, `--enable-spot-restore`, and `--spot-restore-timeout` to `az vmss create` ([Spot VMSS docs](https://learn.microsoft.com/en-us/azure/virtual-machine-scale-sets/use-spot)).

```bash
az vmss create \
  --resource-group "$RG" \
  --name "vmss-spot-workers" \
  --image Ubuntu2204 \
  --orchestration-mode Flexible \
  --single-placement-group false \
  --instance-count 2 \
  --admin-username "$ADMIN_USER" \
  --generate-ssh-keys \
  --priority Spot \
  --eviction-policy Deallocate \
  --max-price -1 \
  --enable-spot-restore True \
  --spot-restore-timeout PT1H
```

---

## 3. Azure Container Instances (ACI)

Azure Container Instances lets you run container groups directly with `az container create`, and the command reference lists `az container create`, `az container logs`, `az container attach`, `az container exec`, `az container start`, `az container stop`, and related commands ([az container](https://learn.microsoft.com/en-us/cli/azure/container?view=azure-cli-latest)).

### 3.1 Launch a public container quickly

The ACI quickstart creates a container with `--resource-group`, `--name`, `--image`, `--dns-name-label`, `--ports`, `--os-type`, `--memory`, and `--cpu` ([ACI quickstart](https://learn.microsoft.com/en-us/azure/container-instances/container-instances-quickstart)).

```bash
export ACI_NAME="aci-hello"
export DNS_LABEL="aci-hello-$RANDOM"

az container create \
  --resource-group "$RG" \
  --name "$ACI_NAME" \
  --image mcr.microsoft.com/azuredocs/aci-helloworld \
  --dns-name-label "$DNS_LABEL" \
  --ports 80 \
  --os-type Linux \
  --cpu 1 \
  --memory 1.5 \
  --restart-policy OnFailure \
  --tags workload=container environment=dev

az container show \
  --resource-group "$RG" \
  --name "$ACI_NAME" \
  --query "{State:instanceView.state,FQDN:ipAddress.fqdn,IP:ipAddress.ip}" \
  --output table
```

Key flags:

- `--image`: public or private container image.
- `--ports`: exposed container port.
- `--dns-name-label`: public DNS label, which must be unique in the region ([ACI quickstart](https://learn.microsoft.com/en-us/azure/container-instances/container-instances-quickstart)).
- `--cpu` and `--memory`: requested container resources.
- `--restart-policy`: use `Always`, `OnFailure`, or `Never` depending on service vs job behavior.

### 3.2 View logs and attach output

Microsoft recommends starting ACI troubleshooting by viewing logs with `az container logs` and streaming output with `az container attach` ([ACI logs docs](https://learn.microsoft.com/en-us/azure/container-instances/container-instances-get-logs)).

```bash
az container logs \
  --resource-group "$RG" \
  --name "$ACI_NAME"

az container attach \
  --resource-group "$RG" \
  --name "$ACI_NAME"
```

### 3.3 Run a one-off or batch-style container job

For a one-off job, set `--restart-policy Never` and provide a command line; `az container create` supports container creation, and ACI logs can be read afterward with `az container logs` ([az container](https://learn.microsoft.com/en-us/cli/azure/container?view=azure-cli-latest), [ACI logs docs](https://learn.microsoft.com/en-us/azure/container-instances/container-instances-get-logs)).

```bash
export JOB_CONTAINER="aci-job-$RANDOM"

az container create \
  --resource-group "$RG" \
  --name "$JOB_CONTAINER" \
  --image mcr.microsoft.com/azure-cli \
  --restart-policy Never \
  --command-line "sh -c 'echo Starting job; date; sleep 20; echo Done'" \
  --cpu 1 \
  --memory 1

# Poll until the container terminates.
az container show \
  --resource-group "$RG" \
  --name "$JOB_CONTAINER" \
  --query "containers[0].instanceView.currentState" \
  --output json

# Read job output.
az container logs -g "$RG" -n "$JOB_CONTAINER"
```

---

## 4. Azure Kubernetes Service (AKS)

AKS creates a managed Kubernetes cluster with `az aks create`, retrieves kubeconfig credentials with `az aks get-credentials`, and then uses `kubectl` to deploy Kubernetes workloads ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).

### 4.1 Create an AKS cluster

The AKS quickstart creates a resource group, then creates a cluster with `az aks create --resource-group --name --node-count --generate-ssh-keys` ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).

```bash
export AKS_NAME="aks-compute-demo"

# Optional: register provider if needed.
az provider register --namespace Microsoft.ContainerService

az aks create \
  --resource-group "$RG" \
  --name "$AKS_NAME" \
  --node-count 1 \
  --generate-ssh-keys \
  --tags workload=kubernetes environment=dev
```

Production clusters typically need more than one node, defined networking, managed identities, Azure Monitor settings, upgrade strategy, and workload identity decisions; the AKS quickstart notes that its one-node cluster saves time and resources and that production commonly uses more nodes ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).

### 4.2 Get credentials and verify nodes

`az aks get-credentials` downloads credentials and configures the Kubernetes CLI to use the cluster ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).

```bash
az aks get-credentials \
  --resource-group "$RG" \
  --name "$AKS_NAME"

kubectl get nodes
```

### 4.3 Deploy a workload

The AKS quickstart deploys Kubernetes objects with `kubectl apply -f` after obtaining credentials ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).

```bash
cat > hello-aks.yaml <<'YAML'
apiVersion: apps/v1
kind: Deployment
metadata:
  name: hello-aks
spec:
  replicas: 2
  selector:
    matchLabels:
      app: hello-aks
  template:
    metadata:
      labels:
        app: hello-aks
    spec:
      containers:
      - name: hello-aks
        image: mcr.microsoft.com/azuredocs/aci-helloworld
        ports:
        - containerPort: 80
---
apiVersion: v1
kind: Service
metadata:
  name: hello-aks
spec:
  type: LoadBalancer
  selector:
    app: hello-aks
  ports:
  - port: 80
    targetPort: 80
YAML

kubectl apply -f hello-aks.yaml
kubectl get pods
kubectl get service hello-aks --watch
```

### 4.4 Scale node pools

AKS node pools are managed with `az aks nodepool`, and `az aks nodepool scale` scales a node pool in a managed Kubernetes cluster ([az aks nodepool](https://learn.microsoft.com/en-us/cli/azure/aks/nodepool?view=azure-cli-latest)).

```bash
# List node pools.
az aks nodepool list \
  --resource-group "$RG" \
  --cluster-name "$AKS_NAME" \
  --output table

# Scale the default system node pool, commonly named nodepool1.
az aks nodepool scale \
  --resource-group "$RG" \
  --cluster-name "$AKS_NAME" \
  --name nodepool1 \
  --node-count 3

# Add a separate user pool for applications.
az aks nodepool add \
  --resource-group "$RG" \
  --cluster-name "$AKS_NAME" \
  --name userpool1 \
  --node-count 2 \
  --mode User \
  --node-vm-size Standard_D4s_v5
```

---

## 5. Azure Batch for parallel and HPC-style jobs

Azure Batch uses a Batch account, optional linked storage account, a pool of compute nodes, a job, and tasks that run on the pool ([Azure Batch CLI quickstart](https://learn.microsoft.com/en-us/azure/batch/quick-create-cli)). The `az batch` reference includes command groups for accounts, pools, jobs, and tasks ([az batch](https://learn.microsoft.com/en-us/cli/azure/batch?view=azure-cli-latest)).

### 5.1 Create Batch prerequisites

The Batch quickstart creates a storage account, creates a Batch account with `az batch account create`, and logs into the Batch account context with `az batch account login --shared-key-auth` ([Azure Batch CLI quickstart](https://learn.microsoft.com/en-us/azure/batch/quick-create-cli)).

```bash
export RANDOM_SUFFIX=$(openssl rand -hex 3)
export BATCH_RG="$RG"
export STORAGE_ACCOUNT="batchstore$RANDOM_SUFFIX"
export BATCH_ACCOUNT="batchacct$RANDOM_SUFFIX"
export POOL_ID="pool$RANDOM_SUFFIX"
export JOB_ID="job$RANDOM_SUFFIX"

az storage account create \
  --resource-group "$BATCH_RG" \
  --name "$STORAGE_ACCOUNT" \
  --location "$LOCATION" \
  --sku Standard_LRS

az batch account create \
  --name "$BATCH_ACCOUNT" \
  --storage-account "$STORAGE_ACCOUNT" \
  --resource-group "$BATCH_RG" \
  --location "$LOCATION"

az batch account login \
  --name "$BATCH_ACCOUNT" \
  --resource-group "$BATCH_RG" \
  --shared-key-auth
```

### 5.2 Create a pool

The Batch quickstart creates a Linux pool with `az batch pool create --id --image --node-agent-sku-id --target-dedicated-nodes --vm-size` ([Azure Batch CLI quickstart](https://learn.microsoft.com/en-us/azure/batch/quick-create-cli)). The `az batch pool` reference notes that pool creation uses either Cloud Services Configuration or Virtual Machine Configuration arguments ([az batch pool](https://learn.microsoft.com/en-us/cli/azure/batch/pool?view=azure-cli-latest)).

```bash
az batch pool create \
  --id "$POOL_ID" \
  --image canonical:0001-com-ubuntu-server-focal:20_04-lts \
  --node-agent-sku-id "batch.node.ubuntu 20.04" \
  --target-dedicated-nodes 2 \
  --vm-size Standard_A1_v2

az batch pool show \
  --pool-id "$POOL_ID" \
  --query "{allocationState: allocationState, dedicated: targetDedicatedNodes}" \
  --output table
```

### 5.3 Create a job and parallel tasks

The Batch quickstart creates a job with `az batch job create --id --pool-id` and then creates tasks with `az batch task create --task-id --job-id --command-line` ([Azure Batch CLI quickstart](https://learn.microsoft.com/en-us/azure/batch/quick-create-cli)). The Batch task reference confirms `az batch task create --job-id` and optional `--command-line` ([az batch task](https://learn.microsoft.com/en-us/cli/azure/batch/task?view=azure-cli-latest)).

```bash
az batch job create \
  --id "$JOB_ID" \
  --pool-id "$POOL_ID"

for i in {1..8}; do
  az batch task create \
    --task-id "task$i" \
    --job-id "$JOB_ID" \
    --command-line "/bin/bash -c 'echo Task $i on $(hostname); printenv | grep AZ_BATCH; sleep 30'"
done

# Check task status.
az batch task list \
  --job-id "$JOB_ID" \
  --query "[].{id:id,state:state,exitCode:executionInfo.exitCode}" \
  --output table

# Show one task's details.
az batch task show \
  --job-id "$JOB_ID" \
  --task-id task1 \
  --output jsonc
```

### 5.4 Batch scaling notes

Batch pools can use dedicated nodes and low-priority nodes; the pool reference includes `--target-dedicated-nodes` and `--target-low-priority-nodes` as pool creation options ([az batch pool](https://learn.microsoft.com/en-us/cli/azure/batch/pool?view=azure-cli-latest)). For large workloads, tune pool size, task slots per node, VM family, storage throughput, retry limits, and cleanup policy.

```bash
# Resize an existing pool.
az batch pool resize \
  --pool-id "$POOL_ID" \
  --target-dedicated-nodes 4
```

---

## 6. Container Apps and serverless options

### 6.1 Deploy a serverless container with `az containerapp up`

`az containerapp up` can create or update a container app and related resources, and Microsoft describes it as the fastest way to deploy Container Apps from an existing image, local source code, or GitHub repository ([Container Apps up](https://learn.microsoft.com/en-us/azure/container-apps/containerapp-up)). The setup flow registers `Microsoft.App` and `Microsoft.OperationalInsights`, and may install or update the `containerapp` extension ([Container Apps up](https://learn.microsoft.com/en-us/azure/container-apps/containerapp-up)).

```bash
az extension add --name containerapp --upgrade
az provider register --namespace Microsoft.App
az provider register --namespace Microsoft.OperationalInsights

export CONTAINERAPP_NAME="ca-hello-$RANDOM"

az containerapp up \
  --name "$CONTAINERAPP_NAME" \
  --resource-group "$RG" \
  --location "$LOCATION" \
  --image mcr.microsoft.com/azuredocs/containerapps-helloworld:latest \
  --ingress external \
  --target-port 80
```

### 6.2 Create an environment and app explicitly

The `az containerapp` command reference lists `az containerapp create` for creating a container app and `az containerapp up` for creating or updating the app and associated resources ([az containerapp](https://learn.microsoft.com/en-us/cli/azure/containerapp?view=azure-cli-latest)). The Container Apps logs reference provides `az containerapp logs show` for past logs and real-time logs with `--follow` ([az containerapp logs](https://learn.microsoft.com/en-us/cli/azure/containerapp/logs?view=azure-cli-latest)).

```bash
export ACA_ENV="aca-env-demo"
export ACA_APP="aca-api-demo"

az containerapp env create \
  --name "$ACA_ENV" \
  --resource-group "$RG" \
  --location "$LOCATION"

az containerapp create \
  --name "$ACA_APP" \
  --resource-group "$RG" \
  --environment "$ACA_ENV" \
  --image mcr.microsoft.com/azuredocs/containerapps-helloworld:latest \
  --target-port 80 \
  --ingress external \
  --cpu 0.5 \
  --memory 1Gi

az containerapp logs show \
  --name "$ACA_APP" \
  --resource-group "$RG" \
  --follow \
  --tail 30
```

### 6.3 Azure Functions mention

Azure Functions is the main event-driven serverless code option; Microsoft’s CLI quickstart creates a resource group, storage account, identity, function app with `az functionapp create`, and deploys code with `func azure functionapp publish` ([Azure Functions CLI quickstart](https://learn.microsoft.com/en-us/azure/azure-functions/create-first-function-cli-python)). Use Functions when the workload is an event handler or HTTP-triggered function rather than a long-running container service.

```bash
# Very abbreviated shape; see the official Functions quickstart for identity and storage role-assignment details.
az extension add --name application-insights
az functionapp list-flexconsumption-locations --output table

az functionapp create \
  --resource-group "AzureFunctionsQuickstart-rg" \
  --name "<APP_NAME>" \
  --flexconsumption-location "<REGION>" \
  --runtime python \
  --runtime-version "<LANGUAGE_VERSION>" \
  --storage-account "<STORAGE_NAME>" \
  --deployment-storage-auth-type UserAssignedIdentity \
  --deployment-storage-auth-value "func-host-storage-user"

func azure functionapp publish "<APP_NAME>"
```

---

## 7. GPU and HPC workloads

Azure GPU-optimized VM sizes are specialized VMs with single, multiple, or fractional GPUs for compute-intensive, graphics-intensive, and visualization workloads ([Azure VM sizes overview](https://learn.microsoft.com/en-us/azure/virtual-machines/sizes/overview)). Microsoft’s VM sizes overview lists GPU families including NC, ND, NG, and NV, with NC and ND families commonly aligned to AI/ML and high-performance compute patterns ([Azure VM sizes overview](https://learn.microsoft.com/en-us/azure/virtual-machines/sizes/overview)).

### 7.1 Discover GPU sizes

```bash
# Search for N-series or specific GPU families in a region.
az vm list-skus \
  --location "$LOCATION" \
  --resource-type virtualMachines \
  --size Standard_N \
  --all \
  --output table
```

### 7.2 Create a GPU VM

N-series VMs backed by NVIDIA GPUs require NVIDIA GPU drivers to use GPU capabilities, and Microsoft provides an NVIDIA GPU Driver Extension and manual driver guidance for Linux N-series VMs ([N-series Linux GPU drivers](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/n-series-driver-setup)).

```bash
export GPU_VM="vm-gpu-demo"

az vm create \
  --resource-group "$RG" \
  --name "$GPU_VM" \
  --image Ubuntu2204 \
  --size Standard_NC4as_T4_v3 \
  --admin-username "$ADMIN_USER" \
  --generate-ssh-keys \
  --public-ip-sku Standard \
  --tags workload=gpu environment=dev

# Install NVIDIA GPU driver extension for supported N-series Linux VMs.
az vm extension set \
  --resource-group "$RG" \
  --vm-name "$GPU_VM" \
  --publisher Microsoft.HpcCompute \
  --name NvidiaGpuDriverLinux

# Verify after driver installation and reboot if needed.
az vm run-command invoke \
  --resource-group "$RG" \
  --name "$GPU_VM" \
  --command-id RunShellScript \
  --scripts "nvidia-smi || true"
```

### 7.3 GPU/HPC considerations

- Check quota before provisioning GPUs because GPU families often have separate vCPU quota limits; the AKS quickstart also flags that subscriptions may need vCPU quota increases for cluster creation ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).
- Choose the family by workload: NC/ND for ML training, inference, and HPC compute; NV/NG for visualization, VDI, encoding, and rendering patterns ([Azure VM sizes overview](https://learn.microsoft.com/en-us/azure/virtual-machines/sizes/overview)).
- Use Azure Batch for large arrays of GPU or CPU tasks when the workflow is many parallel jobs rather than a continuously running service ([Azure Batch CLI quickstart](https://learn.microsoft.com/en-us/azure/batch/quick-create-cli)).
- Use Spot VMs or Spot VMSS only when checkpointing and retry logic can tolerate eviction ([Azure Spot VM CLI](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/spot-cli)).

---

## 8. Best practices

### 8.1 Cost management

Use `az vm deallocate` rather than only `az vm stop` when you want to release VM compute allocation, because Microsoft’s command reference states that deallocation means compute resources are no longer allocated and compute charges no longer apply ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)). Use Spot VMs or Spot VMSS for interruption-tolerant workloads, and set `--eviction-policy` and `--max-price` intentionally ([Azure Spot VM CLI](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/spot-cli), [Spot VMSS docs](https://learn.microsoft.com/en-us/azure/virtual-machine-scale-sets/use-spot)). Configure VM auto-shutdown with `az vm auto-shutdown` for development VMs ([VM auto-shutdown](https://learn.microsoft.com/en-us/azure/virtual-machines/auto-shutdown-vm)).

```bash
# Auto-shutdown at 18:00 local Azure schedule semantics for the VM resource.
az vm auto-shutdown \
  --resource-group "$RG" \
  --name "$VM_NAME" \
  --time 1800

# Turn off the auto-shutdown schedule.
az vm auto-shutdown \
  --resource-group "$RG" \
  --name "$VM_NAME" \
  --off
```

### 8.2 Tag everything

Azure CLI supports `az tag create` to replace all tags on a resource and `az tag update --operation Merge` to add or modify tags without replacing all existing tags ([Azure tag CLI docs](https://learn.microsoft.com/en-us/azure/azure-resource-manager/management/tag-resources-cli)). Tags help with cost allocation, ownership, cleanup, automation, and policy.

```bash
# Get a VM resource ID.
VM_ID=$(az vm show -g "$RG" -n "$VM_NAME" --query id -o tsv)

# Merge tags onto the VM.
az tag update \
  --resource-id "$VM_ID" \
  --operation Merge \
  --tags owner=platform-team costCenter=demo ttl=7d
```

### 8.3 Use managed identity where possible

Prefer managed identities over secrets on VMs, AKS, Functions, and Container Apps where the workload needs Azure resource access. The Linux VM quickstart includes `--assign-identity` in its `az vm create` pattern, and the Azure Functions quickstart uses a user-assigned managed identity for deployment storage authentication ([Linux VM quickstart](https://learn.microsoft.com/en-us/azure/virtual-machines/linux/quick-create-cli), [Azure Functions CLI quickstart](https://learn.microsoft.com/en-us/azure/azure-functions/create-first-function-cli-python)).

```bash
# Assign a system-managed identity to an existing VM.
az vm identity assign \
  --resource-group "$RG" \
  --name "$VM_NAME"

# Show the VM identity principal ID.
az vm identity show \
  --resource-group "$RG" \
  --name "$VM_NAME" \
  --query principalId \
  --output tsv
```

### 8.4 Troubleshooting checklist

- Add `--debug` to any `az` command to see full request and response details; Azure CLI command references list `--debug` as a global parameter ([az group](https://learn.microsoft.com/en-us/cli/azure/group?view=azure-cli-latest)).
- Use `--query` and `--output table/jsonc/tsv` to make results readable; Azure CLI command references list output formats and JMESPath query support ([az group](https://learn.microsoft.com/en-us/cli/azure/group?view=azure-cli-latest)).
- Check resource provider registration for AKS and Container Apps when first using a subscription; Microsoft’s AKS quickstart shows `az provider show` and `az provider register` for `Microsoft.ContainerService`, and Container Apps setup registers `Microsoft.App` and `Microsoft.OperationalInsights` ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli), [Container Apps up](https://learn.microsoft.com/en-us/azure/container-apps/containerapp-up)).
- Inspect VM boot and guest issues with `az vm get-instance-view`, `az vm run-command invoke`, serial console, and boot diagnostics; `az vm run-command invoke` is the CLI path for executing commands on a VM ([az vm](https://learn.microsoft.com/en-us/cli/azure/vm?view=azure-cli-latest)).
- For ACI, start with `az container logs` and `az container attach` as Microsoft recommends for container troubleshooting ([ACI logs docs](https://learn.microsoft.com/en-us/azure/container-instances/container-instances-get-logs)).
- For AKS, verify `kubectl get nodes`, `kubectl get pods -A`, service events, and node pool capacity after `az aks get-credentials` ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).

### 8.5 Cleanup

Resource groups make cleanup easy because deleting the resource group removes contained resources; Microsoft’s resource group CLI docs show `az group delete --name exampleGroup` ([Manage resource groups CLI](https://learn.microsoft.com/en-us/azure/azure-resource-manager/management/manage-resource-groups-cli)). Use `--no-wait --yes` for noninteractive cleanup after verifying the resource group name ([AKS CLI quickstart](https://learn.microsoft.com/en-us/azure/aks/learn/quick-kubernetes-deploy-cli)).

```bash
# Review resources before deletion.
az resource list \
  --resource-group "$RG" \
  --output table

# Delete all demo resources.
az group delete \
  --name "$RG" \
  --yes \
  --no-wait
```

---

## 9. Full copy-paste starter workflow

This workflow provisions a resource group, Linux VM, ACI container, and Container App skeleton, then prints inspection commands. It is intended for a dev sandbox and should be cleaned up when finished.

```bash
set -euo pipefail

export LOCATION="westus2"
export RG="rg-compute-demo-$RANDOM"
export ADMIN_USER="azureuser"

az group create --name "$RG" --location "$LOCATION" --tags project=az-compute-guide environment=dev

# VM
az vm create \
  --resource-group "$RG" \
  --name vm-linux-demo \
  --image Ubuntu2204 \
  --size Standard_B2s \
  --admin-username "$ADMIN_USER" \
  --generate-ssh-keys \
  --public-ip-sku Standard

# ACI
az container create \
  --resource-group "$RG" \
  --name aci-hello \
  --image mcr.microsoft.com/azuredocs/aci-helloworld \
  --ports 80 \
  --os-type Linux \
  --cpu 1 \
  --memory 1.5 \
  --restart-policy OnFailure

# Container Apps
az extension add --name containerapp --upgrade
az provider register --namespace Microsoft.App
az provider register --namespace Microsoft.OperationalInsights
az containerapp up \
  --name ca-hello-demo \
  --resource-group "$RG" \
  --location "$LOCATION" \
  --image mcr.microsoft.com/azuredocs/containerapps-helloworld:latest \
  --ingress external \
  --target-port 80

# Inspect
az vm show -g "$RG" -n vm-linux-demo --show-details --output table
az container show -g "$RG" -n aci-hello --query "{state:instanceView.state,ip:ipAddress.ip,fqdn:ipAddress.fqdn}" --output table
az containerapp show -g "$RG" -n ca-hello-demo --query "properties.configuration.ingress.fqdn" -o tsv

# Cleanup when done:
# az group delete --name "$RG" --yes --no-wait
```
