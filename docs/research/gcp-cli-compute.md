# Research: GCP gcloud Compute Guide

> **Provenance.** Captured 2026-06-26 from a user-supplied hands-on guide
> ("GCP gcloud Compute Guide: Launch Workloads on Google Cloud"), whose command
> syntax is grounded in the official Google Cloud CLI / Compute Engine docs
> (primary-source links are inline below).
>
> **Status:** reference / background. It is the cited basis for the `gcloud`
> command shapes the **GCP cloud-burst provisioner** uses — `GcpCliControlPlane`
> (`src/core/cloud/gcp_cli_control_plane.cpp`); see
> [gcp-provisioner.md](../cloud/gcp-provisioner.md). zetta's burst backend uses only the
> **Managed Instance Group + Spot** slice (§2.7–2.8); the rest (Cloud Run, GKE,
> Batch, Cloud Functions, GPU/TPU) is background for choosing primitives and for
> the Azure/AWS equivalents in
> [cloud-provisioner-pattern.md](../cloud/cloud-provisioner-pattern.md). Examples are
> illustrative — replace project ids, regions, zones, machine types, and service
> account emails with values your project allows.

---

# GCP gcloud Compute Guide: Launch Workloads on Google Cloud

This hands-on guide shows how to use the Google Cloud CLI (`gcloud`) to provision and run workloads across Google Cloud compute services, including Compute Engine, Cloud Run, GKE, Batch, Cloud Functions, GPUs, and TPUs. The Google Cloud CLI is designed to create and manage Google Cloud resources from the command line or automation scripts, including Compute Engine instances, GKE clusters, and other platform resources ([gcloud CLI overview](https://docs.cloud.google.com/sdk/gcloud)).

> **Assumptions used in examples**
>
> - Shell: Bash, zsh, or Cloud Shell.
> - Replace placeholder values like `PROJECT_ID`, `REGION`, `ZONE`, and service account emails before running commands.
> - Commands are written to be copy-pasteable after exporting the variables below.

```bash
export PROJECT_ID="my-gcp-project"
export REGION="us-central1"
export ZONE="us-central1-a"
```

## 1. Getting started with the gcloud CLI

### 1.1 Install the Google Cloud CLI

Cloud Shell already includes the Google Cloud CLI, but local workstations should install the Google Cloud CLI package for their operating system ([Google Cloud CLI install guide](https://docs.cloud.google.com/sdk/docs/install-sdk)). The Google Cloud CLI currently requires Python 3.10 through 3.14 on platforms where a bundled Python is not used ([Google Cloud CLI install guide](https://docs.cloud.google.com/sdk/docs/install-sdk)).

**Debian/Ubuntu example:**

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates gnupg curl

curl https://packages.cloud.google.com/apt/doc/apt-key.gpg \
  | sudo gpg --dearmor -o /usr/share/keyrings/cloud.google.gpg

echo "deb [signed-by=/usr/share/keyrings/cloud.google.gpg] https://packages.cloud.google.com/apt cloud-sdk main" \
  | sudo tee -a /etc/apt/sources.list.d/google-cloud-sdk.list

sudo apt-get update
sudo apt-get install -y google-cloud-cli
```

Initialize after installation:

```bash
gcloud init
```

`gcloud init` authorizes access and configures an initial project and defaults; Google also documents `--console-only` for command-line authorization when a browser flow is not appropriate ([Google Cloud CLI install guide](https://docs.cloud.google.com/sdk/docs/install-sdk)).

### 1.2 Authenticate for CLI use and for local application code

Use `gcloud auth login` for CLI commands run as a human user; this command obtains user credentials through a web-based authorization flow and sets the active account in the current configuration ([gcloud auth login reference](https://docs.cloud.google.com/sdk/gcloud/reference/auth/login)).

```bash
gcloud auth login
```

Use `gcloud auth application-default login` when local application code uses Google Cloud client libraries and Application Default Credentials (ADC); Google documents that this command writes user credentials to the well-known ADC location and does not affect accounts configured by `gcloud auth login` ([gcloud auth application-default login reference](https://docs.cloud.google.com/sdk/gcloud/reference/auth/application-default/login)).

```bash
gcloud auth application-default login
```

### 1.3 Set project, region, and zone defaults

Set the active project so commands do not need `--project` every time; `gcloud config set project PROJECT_ID` is the documented way to set the default project property ([gcloud config set reference](https://docs.cloud.google.com/sdk/gcloud/reference/config/set)).

```bash
gcloud config set project "$PROJECT_ID"
```

Set Compute Engine default region and zone so commands such as VM creation can omit those flags; Google documents `gcloud config set compute/region REGION` and `gcloud config set compute/zone ZONE` for local CLI defaults ([gcloud compute setup guide](https://docs.cloud.google.com/compute/docs/gcloud-compute)).

```bash
gcloud config set compute/region "$REGION"
gcloud config set compute/zone "$ZONE"
```

Verify the active account and configuration; Google documents `gcloud auth list` for credentialed accounts and `gcloud config list` for active configuration properties ([Google Cloud CLI install guide](https://docs.cloud.google.com/sdk/docs/install-sdk), [gcloud auth list reference](https://docs.cloud.google.com/sdk/gcloud/reference/auth/list)).

```bash
gcloud auth list
gcloud config list
gcloud config get-value project
gcloud config get-value compute/region
gcloud config get-value compute/zone
```

### 1.4 Enable required APIs

Most Google Cloud compute services require their service APIs to be enabled first; the Service Usage documentation states that `gcloud services enable SERVICE` enables a service for the current project and accepts multiple service names in one command ([Service Usage enable/disable docs](https://docs.cloud.google.com/service-usage/docs/enable-disable), [gcloud services enable reference](https://docs.cloud.google.com/sdk/gcloud/reference/services/enable)).

```bash
gcloud services enable \
  compute.googleapis.com \
  run.googleapis.com \
  container.googleapis.com \
  batch.googleapis.com \
  cloudfunctions.googleapis.com \
  cloudbuild.googleapis.com \
  artifactregistry.googleapis.com \
  logging.googleapis.com
```

To list enabled APIs:

```bash
gcloud services list --enabled
```

To list APIs you can enable:

```bash
gcloud services list --available
```

## 2. Compute Engine: virtual machines

Compute Engine is Google Cloud's infrastructure-as-a-service VM platform for Linux and Windows workloads, and `gcloud compute` is the CLI command group for managing Compute Engine resources ([Compute Engine documentation](https://docs.cloud.google.com/compute/docs), [gcloud compute guide](https://docs.cloud.google.com/compute/docs/gcloud-compute)).

### 2.1 Find images, image families, and machine types

Compute Engine uses OS images to create VM boot disks, and Google documents `gcloud compute images list` as the CLI command for listing public OS images ([Compute Engine OS images](https://docs.cloud.google.com/compute/docs/images)).

```bash
# List standard public images.
gcloud compute images list

# List Debian images in the debian-cloud image project.
gcloud compute images list \
  --project=debian-cloud \
  --no-standard-images

# Show image families that contain Debian 12 images.
gcloud compute images list \
  --project=debian-cloud \
  --filter='family=debian-12'
```

`gcloud compute machine-types list` lists available machine types, and it can be narrowed by zone using `--zones` or filters ([gcloud compute machine-types list reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/machine-types/list)).

```bash
# List all machine types visible to the project.
gcloud compute machine-types list

# List machine types in one zone.
gcloud compute machine-types list --zones="$ZONE"

# Describe a specific machine type.
gcloud compute machine-types describe e2-medium --zone="$ZONE"
```

### 2.2 Create a basic VM

`gcloud compute instances create` creates Compute Engine VM instances, and Google documents common creation flags such as image/image family, machine type, zone, boot disk options, metadata, and service account settings ([gcloud compute instances create reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/instances/create), [Create and start a Compute Engine instance](https://docs.cloud.google.com/compute/docs/instances/create-start-instance)).

```bash
export VM_NAME="web-1"
export VM_SA="vm-runtime-sa@${PROJECT_ID}.iam.gserviceaccount.com"

gcloud compute instances create "$VM_NAME" \
  --project="$PROJECT_ID" \
  --zone="$ZONE" \
  --machine-type=e2-medium \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --boot-disk-size=20GB \
  --boot-disk-type=pd-balanced \
  --network=default \
  --subnet=default \
  --service-account="$VM_SA" \
  --scopes=https://www.googleapis.com/auth/cloud-platform \
  --tags=http-server \
  --labels=env=dev,app=web \
  --metadata=startup-script='#!/bin/bash
apt-get update
apt-get install -y nginx
systemctl enable --now nginx
'
```

Key flags:

| Flag | Purpose |
|---|---|
| `--machine-type` | Selects the VM shape, such as `e2-medium`, `n2-standard-4`, or GPU-optimized shapes. |
| `--image-family` and `--image-project` | Select the latest non-deprecated image from an image family, such as `debian-12` in `debian-cloud`. |
| `--boot-disk-size` and `--boot-disk-type` | Configure the boot disk capacity and disk class. |
| `--network` and `--subnet` | Attach the VM NIC to a VPC network and subnet. |
| `--service-account` and `--scopes` | Control the service account identity and OAuth scopes used by the VM. |
| `--metadata=startup-script=...` | Pass a startup script that runs at boot time. |
| `--tags` | Apply network tags that firewall rules can target. |
| `--labels` | Apply resource labels for cost allocation and inventory. |

If your subnet does not allow inbound HTTP, create a firewall rule for tagged instances:

```bash
gcloud compute firewall-rules create allow-http-to-web \
  --network=default \
  --allow=tcp:80 \
  --target-tags=http-server \
  --description="Allow HTTP to web VMs"
```

### 2.3 Startup scripts and metadata

Startup scripts are files or inline commands that run when a VM boots, and Compute Engine supports startup scripts on Linux and Windows VMs ([Compute Engine startup scripts](https://docs.cloud.google.com/compute/docs/instances/startup-scripts)). Google documents metadata keys such as `startup-script` and `startup-script-url`, and the CLI supports `--metadata` and `--metadata-from-file` for metadata values ([gcloud compute instances add-metadata reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/instances/add-metadata)).

Create a script file and pass it during VM creation:

```bash
cat > startup.sh <<'SCRIPT'
#!/bin/bash
set -euxo pipefail
apt-get update
apt-get install -y nginx
hostname > /var/www/html/index.html
SCRIPT

gcloud compute instances create web-from-file \
  --zone="$ZONE" \
  --machine-type=e2-small \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --metadata-from-file=startup-script=startup.sh
```

Update metadata on an existing VM and restart it if you need the startup script to run on next boot:

```bash
gcloud compute instances add-metadata web-from-file \
  --zone="$ZONE" \
  --metadata-from-file=startup-script=startup.sh

gcloud compute instances stop web-from-file --zone="$ZONE"
gcloud compute instances start web-from-file --zone="$ZONE"
```

### 2.4 OS Login considerations

OS Login lets you manage Linux VM SSH access through IAM by setting `enable-oslogin=TRUE` in project or instance metadata ([Set up OS Login](https://docs.cloud.google.com/compute/docs/oslogin/set-up-oslogin)). Google documents enabling OS Login at VM creation with `--metadata enable-oslogin=TRUE` ([Set up OS Login](https://docs.cloud.google.com/compute/docs/oslogin/set-up-oslogin)).

Enable OS Login for one VM at creation:

```bash
gcloud compute instances create oslogin-vm \
  --zone="$ZONE" \
  --machine-type=e2-small \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --metadata=enable-oslogin=TRUE
```

Enable OS Login for all VMs in a project:

```bash
gcloud compute project-info add-metadata \
  --metadata=enable-oslogin=TRUE
```

Disable OS Login for one VM even if it is enabled project-wide:

```bash
gcloud compute instances add-metadata oslogin-vm \
  --zone="$ZONE" \
  --metadata=enable-oslogin=FALSE
```

### 2.5 Connect to VMs and run remote commands

`gcloud compute ssh` wraps SSH, handles authentication, translates the instance name to an IP address, and can run a remote command with `--command` ([gcloud compute ssh reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/ssh)).

```bash
# Interactive SSH.
gcloud compute ssh "$VM_NAME" --zone="$ZONE"

# Run a one-off command.
gcloud compute ssh "$VM_NAME" \
  --zone="$ZONE" \
  --command="uname -a && systemctl status nginx --no-pager"

# Use IAP tunneling when the VM has no external IP and IAP is configured.
gcloud compute ssh "$VM_NAME" \
  --zone="$ZONE" \
  --tunnel-through-iap
```

If a custom VPC firewall blocks port 22, Google notes that you might need a firewall rule allowing TCP port 22 to use `gcloud compute ssh` ([gcloud compute ssh reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/ssh)).

```bash
gcloud compute firewall-rules create allow-ssh \
  --network=default \
  --allow=tcp:22 \
  --source-ranges=203.0.113.0/24 \
  --target-tags=ssh-allowed
```

### 2.6 Start, stop, and delete VMs

`gcloud compute instances stop` stops one or more VMs in a zone, and stopping a VM retains attached disks, configuration settings, IP addresses, MAC addresses, and metadata ([Stop or restart a Compute Engine instance](https://docs.cloud.google.com/compute/docs/instances/stop-start-instance)).

```bash
gcloud compute instances stop "$VM_NAME" --zone="$ZONE"
gcloud compute instances start "$VM_NAME" --zone="$ZONE"
```

`gcloud compute instances delete` deletes one or more VMs and supports preserving or forcing deletion of attached disks with `--keep-disks` or `--delete-disks` ([gcloud compute instances delete reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/instances/delete), [Delete a Compute Engine instance](https://docs.cloud.google.com/compute/docs/instances/deleting-instance)).

```bash
# Delete the VM and follow each disk's auto-delete setting.
gcloud compute instances delete "$VM_NAME" --zone="$ZONE"

# Delete the VM but keep boot and data disks.
gcloud compute instances delete "$VM_NAME" \
  --zone="$ZONE" \
  --keep-disks=all

# Delete the VM and force deletion of all attached persistent disks.
gcloud compute instances delete "$VM_NAME" \
  --zone="$ZONE" \
  --delete-disks=all
```

### 2.7 Managed Instance Groups (MIGs) and autoscaling

Managed Instance Groups (MIGs) operate applications on multiple identical VMs and provide automated capabilities such as autoscaling, autohealing, regional deployment, and automatic updates ([Compute Engine instance groups](https://docs.cloud.google.com/compute/docs/instance-groups)). Autoscaling is a MIG feature that adds or removes VMs based on signals such as CPU utilization, load balancing capacity, Monitoring metrics, schedules, or queue-based workloads ([Compute Engine autoscaling](https://docs.cloud.google.com/compute/docs/autoscaler)).

Create an instance template:

```bash
gcloud compute instance-templates create web-template \
  --machine-type=e2-micro \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --tags=http-server \
  --labels=env=dev,app=web \
  --metadata=startup-script='#!/bin/bash
apt-get update
apt-get install -y nginx
systemctl enable --now nginx
'
```

Create a zonal MIG from the template; `gcloud compute instance-groups managed create` is the documented command for creating Compute Engine MIGs ([gcloud compute instance-groups managed create reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/instance-groups/managed/create)).

```bash
gcloud compute instance-groups managed create web-mig \
  --zone="$ZONE" \
  --template=web-template \
  --size=2
```

Configure CPU-based autoscaling; `gcloud compute instance-groups managed set-autoscaling` sets autoscaling parameters on a MIG ([gcloud compute instance-groups managed set-autoscaling reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/instance-groups/managed/set-autoscaling)).

```bash
gcloud compute instance-groups managed set-autoscaling web-mig \
  --zone="$ZONE" \
  --min-num-replicas=2 \
  --max-num-replicas=5 \
  --target-cpu-utilization=0.60 \
  --cool-down-period=60
```

Resize, inspect, and disable autoscaling:

```bash
gcloud compute instance-groups managed resize web-mig \
  --zone="$ZONE" \
  --size=3

gcloud compute instance-groups managed describe web-mig \
  --zone="$ZONE"

gcloud compute instance-groups managed stop-autoscaling web-mig \
  --zone="$ZONE"
```

### 2.8 Spot and preemptible VMs for lower-cost fault-tolerant workloads

Spot VMs use the Spot provisioning model, and Google documents `--provisioning-model=SPOT` as the gcloud flag for creating Spot VMs ([Create and use Spot VMs](https://docs.cloud.google.com/compute/docs/instances/create-use-spot)). Spot VMs have no guaranteed runtime and can be stopped or deleted by Compute Engine when capacity is needed, so they are appropriate for stateless, fault-tolerant, or checkpointed workloads ([Create and use Spot VMs](https://docs.cloud.google.com/compute/docs/instances/create-use-spot)).

```bash
cat > shutdown.sh <<'SCRIPT'
#!/bin/bash
logger "Spot VM is shutting down; save checkpoints here."
SCRIPT

gcloud compute instances create spot-worker-1 \
  --zone="$ZONE" \
  --machine-type=e2-standard-4 \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --provisioning-model=SPOT \
  --instance-termination-action=STOP \
  --metadata-from-file=shutdown-script=shutdown.sh \
  --labels=env=dev,workload=batch,cost=spot
```

The legacy `--preemptible` flag still appears in the `gcloud compute instances create` reference, but Google positions Spot VMs as the modern provisioning model for discounted interruptible capacity ([gcloud compute instances create reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/instances/create), [Compute Engine provisioning models](https://docs.cloud.google.com/compute/docs/instances/provisioning-models)).

```bash
# Legacy-style preemptible VM example.
gcloud compute instances create preemptible-worker-1 \
  --zone="$ZONE" \
  --machine-type=e2-standard-4 \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --preemptible \
  --no-restart-on-failure \
  --maintenance-policy=TERMINATE
```

## 3. Cloud Run: serverless containerized workloads

Cloud Run is a fully managed application platform for containers that are invoked by requests or events ([Cloud Run documentation](https://docs.cloud.google.com/run/docs)). `gcloud run deploy` creates or updates a Cloud Run service ([gcloud run deploy reference](https://docs.cloud.google.com/sdk/gcloud/reference/run/deploy)).

### 3.1 Deploy a container as a Cloud Run service

```bash
export SERVICE="hello-run"

gcloud run deploy "$SERVICE" \
  --image=us-docker.pkg.dev/cloudrun/container/hello:latest \
  --region="$REGION" \
  --allow-unauthenticated \
  --port=8080 \
  --set-env-vars=APP_ENV=dev \
  --labels=env=dev,app=hello
```

Useful Cloud Run service commands:

```bash
# List services.
gcloud run services list --region="$REGION"

# Describe the service.
gcloud run services describe "$SERVICE" --region="$REGION"

# Update resource settings.
gcloud run services update "$SERVICE" \
  --region="$REGION" \
  --cpu=1 \
  --memory=512Mi \
  --min-instances=0 \
  --max-instances=10

# Read recent service logs.
gcloud run services logs read "$SERVICE" \
  --region="$REGION" \
  --limit=50
```

Cloud Run lets you deploy from source with `gcloud run deploy SERVICE --source .`, and the documentation notes that this path uses buildpacks or a Dockerfile and can eliminate a separate `gcloud builds submit` step for simple deployments ([Deploy services from source code](https://docs.cloud.google.com/run/docs/deploying-source-code)).

```bash
# From a local source directory containing app code or a Dockerfile.
gcloud run deploy source-run \
  --source=. \
  --region="$REGION" \
  --allow-unauthenticated
```

### 3.2 Cloud Run Jobs for one-off and batch containers

Cloud Run Jobs run containers to completion and are appropriate for administrative tasks, data processing, or batch work that does not serve HTTP requests ([Create Cloud Run jobs](https://docs.cloud.google.com/run/docs/create-jobs)). `gcloud run jobs create` creates a job, and `gcloud run jobs execute` starts a job execution ([Execute Cloud Run jobs](https://docs.cloud.google.com/run/docs/execute/jobs)).

```bash
export JOB="hello-job"

gcloud run jobs create "$JOB" \
  --image=us-docker.pkg.dev/cloudrun/container/job:latest \
  --region="$REGION" \
  --tasks=5 \
  --max-retries=2 \
  --task-timeout=10m \
  --set-env-vars=JOB_ENV=dev
```

Execute the job and wait for completion:

```bash
gcloud run jobs execute "$JOB" \
  --region="$REGION" \
  --wait
```

Create and execute immediately:

```bash
gcloud run jobs create immediate-job \
  --image=us-docker.pkg.dev/cloudrun/container/job:latest \
  --region="$REGION" \
  --execute-now
```

Cloud Run job executions write logs to Cloud Logging, and Google documents using `gcloud beta run jobs execute JOB_NAME --tail --region=REGION` to tail logs immediately after launch ([Execute Cloud Run jobs](https://docs.cloud.google.com/run/docs/execute/jobs), [Cloud Run logging](https://docs.cloud.google.com/run/docs/logging)).

```bash
# Tail logs while executing; beta command per current docs.
gcloud beta run jobs execute "$JOB" \
  --region="$REGION" \
  --tail

# Read logs from Cloud Logging for a Cloud Run Job resource.
gcloud logging read \
  'resource.type="cloud_run_job" AND resource.labels.job_name="hello-job"' \
  --limit=50 \
  --format='value(textPayload)'
```

## 4. Google Kubernetes Engine (GKE)

GKE runs Kubernetes clusters on Google Cloud and supports Autopilot and Standard operation modes; in Autopilot, Google manages nodes, scaling, security, and other infrastructure configuration, while Standard gives more direct node control ([GKE Autopilot overview](https://docs.cloud.google.com/kubernetes-engine/docs/concepts/autopilot-overview)).

### 4.1 Create an Autopilot cluster

Google documents `gcloud container clusters create-auto CLUSTER_NAME --location=LOCATION --project=PROJECT_ID` as the minimum gcloud command to create an Autopilot cluster ([Create an Autopilot cluster](https://docs.cloud.google.com/kubernetes-engine/docs/how-to/creating-an-autopilot-cluster)).

```bash
export CLUSTER="autopilot-cluster"

gcloud container clusters create-auto "$CLUSTER" \
  --location="$REGION" \
  --project="$PROJECT_ID"
```

Google recommends specifying a minimally privileged IAM service account for nodes instead of the Compute Engine default service account when creating Autopilot clusters ([Create an Autopilot cluster](https://docs.cloud.google.com/kubernetes-engine/docs/how-to/creating-an-autopilot-cluster)).

```bash
export GKE_NODE_SA="gke-node-sa@${PROJECT_ID}.iam.gserviceaccount.com"

gcloud container clusters create-auto autopilot-secure \
  --location="$REGION" \
  --project="$PROJECT_ID" \
  --service-account="$GKE_NODE_SA"
```

### 4.2 Create a Standard cluster and node pools

`gcloud container clusters create` is the gcloud reference command for creating GKE clusters in Standard mode ([gcloud container clusters create reference](https://docs.cloud.google.com/sdk/gcloud/reference/container/clusters/create)).

```bash
export STD_CLUSTER="standard-cluster"

gcloud container clusters create "$STD_CLUSTER" \
  --region="$REGION" \
  --num-nodes=1 \
  --machine-type=e2-standard-4 \
  --release-channel=regular \
  --labels=env=dev,cluster=standard
```

`gcloud container node-pools create` creates node pools in a running GKE cluster and supports options such as node count and machine type ([gcloud container node-pools create reference](https://docs.cloud.google.com/sdk/gcloud/reference/container/node-pools/create)).

```bash
gcloud container node-pools create cpu-pool \
  --cluster="$STD_CLUSTER" \
  --region="$REGION" \
  --num-nodes=2 \
  --machine-type=e2-standard-4 \
  --enable-autoscaling \
  --min-nodes=1 \
  --max-nodes=5 \
  --labels=pool=cpu,env=dev
```

### 4.3 Get cluster credentials and deploy workloads with kubectl

`gcloud container clusters get-credentials` updates kubeconfig with cluster endpoint and credential information so `kubectl` can talk to a GKE cluster ([gcloud container clusters get-credentials reference](https://docs.cloud.google.com/sdk/gcloud/reference/container/clusters/get-credentials)).

```bash
gcloud container clusters get-credentials "$CLUSTER" \
  --location="$REGION" \
  --project="$PROJECT_ID"
```

Deploy a sample Kubernetes workload:

```bash
kubectl create deployment hello \
  --image=us-docker.pkg.dev/google-samples/containers/gke/hello-app:1.0

kubectl expose deployment hello \
  --type=LoadBalancer \
  --port=80 \
  --target-port=8080

kubectl get deployments
kubectl get services
kubectl logs deployment/hello
```

Scale the deployment:

```bash
kubectl scale deployment hello --replicas=3
kubectl rollout status deployment/hello
```

Clean up the Kubernetes app:

```bash
kubectl delete service hello
kubectl delete deployment hello
```

## 5. Google Cloud Batch and HPC jobs

Batch is a managed batch service for defining a workload and resource requirements, after which Batch queues, schedules, and executes the job ([Batch job creation overview](https://docs.cloud.google.com/batch/docs/create-run-job)). Batch setup requires the Batch, Compute Engine, and Cloud Logging APIs according to Google’s Batch getting started documentation ([Batch getting started](https://docs.cloud.google.com/batch/docs/get-started)).

### 5.1 Submit a simple script job

The Batch documentation uses a JSON job configuration with `taskGroups`, `taskSpec`, runnables, allocation policy, labels, and a logs policy, then submits it with `gcloud batch jobs submit JOB_NAME --location LOCATION --config JSON_CONFIGURATION_FILE` ([Create and run a basic Batch job](https://docs.cloud.google.com/batch/docs/create-run-basic-job), [gcloud batch jobs submit reference](https://docs.cloud.google.com/sdk/gcloud/reference/batch/jobs/submit)).

```bash
cat > batch-hello.json <<'JSON'
{
  "taskGroups": [
    {
      "taskSpec": {
        "runnables": [
          {
            "script": {
              "text": "echo Hello world! This is task ${BATCH_TASK_INDEX} of ${BATCH_TASK_COUNT}."
            }
          }
        ],
        "computeResource": {
          "cpuMilli": 2000,
          "memoryMib": 2048
        },
        "maxRetryCount": 2,
        "maxRunDuration": "3600s"
      },
      "taskCount": 4,
      "parallelism": 2
    }
  ],
  "allocationPolicy": {
    "instances": [
      {
        "policy": {
          "machineType": "e2-standard-2"
        }
      }
    ]
  },
  "labels": {
    "env": "dev",
    "workload": "batch"
  },
  "logsPolicy": {
    "destination": "CLOUD_LOGGING"
  }
}
JSON

gcloud batch jobs submit example-script-job \
  --location="$REGION" \
  --config=batch-hello.json
```

Inspect the job:

```bash
gcloud batch jobs describe example-script-job --location="$REGION"
gcloud batch jobs list --location="$REGION"
```

Read recent Batch task logs from Cloud Logging:

```bash
gcloud logging read \
  'resource.type="batch_task" AND labels."batch.googleapis.com/job_name"="example-script-job"' \
  --limit=50 \
  --format='value(textPayload)'
```

### 5.2 Use Spot capacity for Batch jobs

Batch job allocation policies can request Spot capacity by setting a VM provisioning model in the job configuration, as shown in Google’s Batch example job that uses `provisioningModel: "SPOT"` ([Create and run an example Batch job](https://docs.cloud.google.com/batch/docs/create-run-example-job)).

```bash
cat > batch-spot.json <<'JSON'
{
  "taskGroups": [
    {
      "taskSpec": {
        "runnables": [
          {
            "script": {
              "text": "python3 - <<'PY'\nprint('run fault-tolerant batch work here')\nPY"
            }
          }
        ],
        "computeResource": {
          "cpuMilli": 4000,
          "memoryMib": 4096
        },
        "maxRetryCount": 3,
        "maxRunDuration": "7200s"
      },
      "taskCount": 10,
      "parallelism": 5
    }
  ],
  "allocationPolicy": {
    "instances": [
      {
        "policy": {
          "machineType": "e2-standard-4",
          "provisioningModel": "SPOT"
        }
      }
    ]
  },
  "logsPolicy": {
    "destination": "CLOUD_LOGGING"
  }
}
JSON

gcloud batch jobs submit spot-batch-job \
  --location="$REGION" \
  --config=batch-spot.json
```

### 5.3 GPU Batch jobs

For GPU Batch jobs, Google documents `gcloud batch jobs submit JOB_NAME --location LOCATION --config JSON_CONFIGURATION_FILE` and recommends setting `installGpuDrivers` to `true` when Batch can install required NVIDIA drivers for the job ([Create and run a Batch job that uses GPUs](https://docs.cloud.google.com/batch/docs/create-run-job-gpus)).

```bash
cat > batch-gpu.json <<'JSON'
{
  "taskGroups": [
    {
      "taskSpec": {
        "runnables": [
          {
            "script": {
              "text": "nvidia-smi && echo GPU job started"
            }
          }
        ],
        "computeResource": {
          "cpuMilli": 4000,
          "memoryMib": 16384
        },
        "maxRetryCount": 1,
        "maxRunDuration": "3600s"
      },
      "taskCount": 1,
      "parallelism": 1
    }
  ],
  "allocationPolicy": {
    "instances": [
      {
        "installGpuDrivers": true,
        "policy": {
          "machineType": "g2-standard-4",
          "accelerators": [
            {
              "type": "nvidia-l4",
              "count": 1
            }
          ]
        }
      }
    ]
  },
  "logsPolicy": {
    "destination": "CLOUD_LOGGING"
  }
}
JSON

gcloud batch jobs submit gpu-batch-job \
  --location="$REGION" \
  --config=batch-gpu.json
```

## 6. Serverless options: Cloud Functions and Cloud Run

Cloud Functions is now presented in Google documentation as Cloud Run functions, and Google notes that functions created with the Cloud Functions v2 API can be managed with Cloud Run Admin API and Cloud Functions v2 API ([Cloud Run functions comparison](https://docs.cloud.google.com/run/docs/functions/comparison)). `gcloud functions deploy` creates or updates a Google Cloud Function and supports `--gen2` for second-generation functions ([gcloud functions deploy reference](https://docs.cloud.google.com/sdk/gcloud/reference/functions/deploy)).

### 6.1 Deploy an HTTP Cloud Function

Example source files:

```bash
mkdir -p hello-function
cd hello-function

cat > package.json <<'JSON'
{
  "dependencies": {
    "@google-cloud/functions-framework": "^3.0.0"
  }
}
JSON

cat > index.js <<'JS'
exports.helloHttp = (req, res) => {
  res.send('Hello from Cloud Run functions');
};
JS
```

Deploy with gcloud:

```bash
gcloud functions deploy hello-http \
  --gen2 \
  --runtime=nodejs22 \
  --region="$REGION" \
  --source=. \
  --entry-point=helloHttp \
  --trigger-http \
  --allow-unauthenticated
```

Call or inspect the function:

```bash
gcloud functions describe hello-http --gen2 --region="$REGION"
gcloud functions call hello-http --region="$REGION" --data='{}'
```

Use Cloud Run when you want to deploy a full container with explicit container runtime, CPU, memory, concurrency, min/max instances, networking, and revision controls; use Cloud Functions when a single function entry point and event trigger are the primary abstraction ([Cloud Run documentation](https://docs.cloud.google.com/run/docs), [Cloud Run functions comparison](https://docs.cloud.google.com/run/docs/functions/comparison)).

## 7. GPU, TPU, and ML/HPC compute workloads

Google Cloud supports GPU machine types for AI, ML, HPC, graphics, and inference workloads, including accelerator-optimized A-series and G-series machine types where GPUs are attached automatically, and N1 machine types where supported GPUs can be attached during or after VM creation ([GPU machine types](https://docs.cloud.google.com/compute/docs/gpus), [Machine families resource guide](https://docs.cloud.google.com/compute/docs/machine-resource)).

### 7.1 List GPU accelerator types

```bash
# List accelerator types available in a zone.
gcloud compute accelerator-types list --filter="zone:($ZONE)"
```

### 7.2 Create an N1 VM with an attached GPU

For VMs with GPUs, Google documents setting `--maintenance-policy=TERMINATE` because GPU VMs cannot live migrate ([Create GPU VM](https://docs.cloud.google.com/compute/docs/gpus/create-gpu-vm-accelerator-optimized)).

```bash
gcloud compute instances create gpu-n1-t4 \
  --zone="$ZONE" \
  --machine-type=n1-standard-4 \
  --accelerator=type=nvidia-tesla-t4,count=1 \
  --maintenance-policy=TERMINATE \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --boot-disk-size=100GB \
  --boot-disk-type=pd-balanced \
  --labels=env=dev,workload=gpu
```

Install NVIDIA drivers after provisioning unless you use an image or service workflow that already installs them; Google’s GPU documentation states that GPU drivers must be installed before the VM can use the GPU ([Create GPU VM](https://docs.cloud.google.com/compute/docs/gpus/create-gpu-vm-accelerator-optimized)).

### 7.3 Create an accelerator-optimized G2 VM

G2 machine types have NVIDIA L4 GPUs automatically attached and are used for cost-optimized inference, graphics-intensive workloads, and HPC workloads ([GPU machine types](https://docs.cloud.google.com/compute/docs/gpus)).

```bash
gcloud compute instances create gpu-g2-l4 \
  --zone="$ZONE" \
  --machine-type=g2-standard-4 \
  --maintenance-policy=TERMINATE \
  --image-family=rocky-linux-8-optimized-gcp \
  --image-project=rocky-linux-cloud \
  --boot-disk-size=200GB \
  --boot-disk-type=pd-ssd \
  --labels=env=dev,workload=inference
```

### 7.4 Create an accelerator-optimized A2/A3 VM

A2 machine types use NVIDIA A100 GPUs, and A3 machine types use NVIDIA H100 GPUs for high-performance training and inference workloads ([GPU machine types](https://docs.cloud.google.com/compute/docs/gpus)).

```bash
gcloud compute instances create gpu-a2 \
  --zone="$ZONE" \
  --machine-type=a2-highgpu-1g \
  --maintenance-policy=TERMINATE \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --boot-disk-size=200GB \
  --boot-disk-type=pd-ssd \
  --labels=env=dev,workload=training
```

Use Spot for fault-tolerant GPU jobs where interruption is acceptable:

```bash
gcloud compute instances create gpu-spot-g2 \
  --zone="$ZONE" \
  --machine-type=g2-standard-4 \
  --maintenance-policy=TERMINATE \
  --provisioning-model=SPOT \
  --instance-termination-action=STOP \
  --image-family=rocky-linux-8-optimized-gcp \
  --image-project=rocky-linux-cloud \
  --boot-disk-size=200GB \
  --boot-disk-type=pd-ssd
```

### 7.5 Cloud TPUs

Cloud TPU VM nodes are managed with `gcloud compute tpus tpu-vm`, and `gcloud compute tpus tpu-vm create` creates a new Cloud TPU VM node with flags such as `--zone`, `--accelerator-type`, and `--version` ([gcloud compute tpus tpu-vm create reference](https://docs.cloud.google.com/sdk/gcloud/reference/compute/tpus/tpu-vm/create)).

```bash
export TPU_NAME="my-tpu"
export TPU_ZONE="us-central2-b"
export TPU_ACCELERATOR="v5litepod-8"
export TPU_VERSION="tpu-vm-tf-2.17.1-pjrt"

gcloud compute tpus tpu-vm create "$TPU_NAME" \
  --zone="$TPU_ZONE" \
  --accelerator-type="$TPU_ACCELERATOR" \
  --version="$TPU_VERSION" \
  --metadata=startup-script='#!/bin/bash
pip3 install --upgrade pip
'
```

Manage TPU VMs with the `tpu-vm` subcommands for SSH, list, describe, stop, start, and delete ([Manage TPU resources](https://docs.cloud.google.com/tpu/docs/managing-tpus-tpu-vm)).

```bash
gcloud compute tpus tpu-vm ssh "$TPU_NAME" --zone="$TPU_ZONE"
gcloud compute tpus tpu-vm list --zone="$TPU_ZONE"
gcloud compute tpus tpu-vm describe "$TPU_NAME" --zone="$TPU_ZONE"
gcloud compute tpus tpu-vm stop "$TPU_NAME" --zone="$TPU_ZONE"
gcloud compute tpus tpu-vm start "$TPU_NAME" --zone="$TPU_ZONE"
gcloud compute tpus tpu-vm delete "$TPU_NAME" --zone="$TPU_ZONE" --quiet
```

## 8. Best practices and operational patterns

### 8.1 Choose the right compute service

| Workload pattern | Recommended starting point | Why |
|---|---|---|
| Long-running VM app with full OS control | Compute Engine VM or MIG | Best when you need OS-level configuration, custom daemons, or legacy apps. |
| Stateless HTTP container | Cloud Run service | Fully managed container runtime with request-based scaling. |
| One-off or scheduled container task | Cloud Run Job | Serverless run-to-completion containers. |
| Kubernetes platform or multi-service cluster | GKE Autopilot first, Standard when node control is required | Autopilot reduces node operations; Standard provides more node-pool control. |
| Large parallel batch or HPC | Google Cloud Batch | Managed scheduling and execution for task arrays and VM allocation policies. |
| Single event-driven function | Cloud Run functions / Cloud Functions | Function-level abstraction with HTTP or event triggers. |
| ML training or high-performance inference | GPU VMs, GKE GPU nodes, Batch GPU jobs, or TPUs | Choose based on framework, scale, queueing needs, and accelerator type. |

### 8.2 Cost management

Stop idle VMs when you expect to reuse disks and configuration, but delete VMs and unused disks when they are no longer needed because deleting a VM stops it and can delete or preserve attached disks depending on disk settings and CLI flags ([Stop or restart a Compute Engine instance](https://docs.cloud.google.com/compute/docs/instances/stop-start-instance), [Delete a Compute Engine instance](https://docs.cloud.google.com/compute/docs/instances/deleting-instance)).

```bash
# Stop dev VMs nightly.
gcloud compute instances stop dev-vm-1 dev-vm-2 --zone="$ZONE"

# Delete completed experiment VMs and their attached disks.
gcloud compute instances delete experiment-vm \
  --zone="$ZONE" \
  --delete-disks=all
```

Use Spot VMs or Spot Batch allocation for fault-tolerant workloads, because Spot VMs use discounted interruptible capacity and can be preempted when Google Cloud needs capacity ([Create and use Spot VMs](https://docs.cloud.google.com/compute/docs/instances/create-use-spot)).

```bash
gcloud compute instances create cheap-worker \
  --zone="$ZONE" \
  --machine-type=e2-standard-2 \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --provisioning-model=SPOT \
  --instance-termination-action=STOP
```

Use autoscaling for MIGs or GKE node pools so capacity can shrink when demand falls; Google documents MIG autoscaling as a way to add or delete VMs based on load and reduce costs when resource needs are lower ([Compute Engine autoscaling](https://docs.cloud.google.com/compute/docs/autoscaler)).

```bash
gcloud compute instance-groups managed set-autoscaling web-mig \
  --zone="$ZONE" \
  --min-num-replicas=1 \
  --max-num-replicas=10 \
  --target-cpu-utilization=0.60
```

### 8.3 Labels and tagging

Labels help with billing allocation, filtering, and ownership tracking, while Compute Engine network tags can be used as firewall rule targets.

```bash
# Add or update labels on a VM.
gcloud compute instances add-labels web-1 \
  --zone="$ZONE" \
  --labels=env=prod,owner=platform,app=web

# Add network tags to a VM.
gcloud compute instances add-tags web-1 \
  --zone="$ZONE" \
  --tags=http-server,ssh-allowed

# Filter resources by label.
gcloud compute instances list \
  --filter='labels.env=prod AND labels.app=web'
```

Use a consistent label taxonomy:

```text
env=dev|staging|prod
owner=team-or-email
app=application-name
cost-center=finance-code
managed-by=gcloud|terraform|gke|batch
```

### 8.4 IAM, authentication, and service accounts

Use user credentials for human `gcloud` operations and service accounts for workloads; Google’s gcloud authentication documentation distinguishes user login, workload identity federation, workforce identity federation, service account key files, and service account impersonation patterns ([Authenticate for the gcloud CLI](https://docs.cloud.google.com/sdk/docs/authenticate)).

Prefer service account impersonation over long-lived service account keys when possible:

```bash
export DEPLOYER_SA="deployer@${PROJECT_ID}.iam.gserviceaccount.com"

gcloud compute instances list \
  --impersonate-service-account="$DEPLOYER_SA"
```

Create a dedicated VM runtime service account and grant only the roles the workload needs:

```bash
gcloud iam service-accounts create vm-runtime-sa \
  --display-name="VM runtime service account"

gcloud projects add-iam-policy-binding "$PROJECT_ID" \
  --member="serviceAccount:vm-runtime-sa@${PROJECT_ID}.iam.gserviceaccount.com" \
  --role="roles/logging.logWriter"
```

For GKE Autopilot, Google recommends specifying a minimally privileged IAM service account for nodes instead of the Compute Engine default service account ([Create an Autopilot cluster](https://docs.cloud.google.com/kubernetes-engine/docs/how-to/creating-an-autopilot-cluster)).

```bash
gcloud container clusters create-auto autopilot-lp \
  --location="$REGION" \
  --service-account="gke-node-sa@${PROJECT_ID}.iam.gserviceaccount.com"
```

### 8.5 Cleanup commands

Delete Cloud Run services and jobs when finished:

```bash
gcloud run services delete "$SERVICE" --region="$REGION" --quiet
gcloud run jobs delete "$JOB" --region="$REGION" --quiet
```

Delete GKE clusters when finished:

```bash
gcloud container clusters delete "$CLUSTER" \
  --location="$REGION" \
  --quiet

gcloud container clusters delete "$STD_CLUSTER" \
  --region="$REGION" \
  --quiet
```

Delete Batch jobs and Compute Engine resources when finished:

```bash
gcloud batch jobs delete example-script-job \
  --location="$REGION" \
  --quiet

gcloud compute instance-groups managed delete web-mig \
  --zone="$ZONE" \
  --quiet

gcloud compute instance-templates delete web-template --quiet
```

Delete GPU and TPU resources when finished:

```bash
gcloud compute instances delete gpu-n1-t4 gpu-g2-l4 gpu-a2 \
  --zone="$ZONE" \
  --delete-disks=all \
  --quiet

gcloud compute tpus tpu-vm delete "$TPU_NAME" \
  --zone="$TPU_ZONE" \
  --quiet
```

### 8.6 Common troubleshooting

| Symptom | Likely cause | Diagnostic commands | Fix |
|---|---|---|---|
| `API has not been used` or API disabled error | Required API is not enabled | `gcloud services list --enabled` | `gcloud services enable SERVICE.googleapis.com` |
| `Permission denied` from gcloud | Active user lacks IAM permissions | `gcloud auth list`; `gcloud config list` | Switch account, set project, or request least-privilege role. |
| VM SSH fails | Firewall, OS Login IAM, missing external IP, or IAP setup | `gcloud compute ssh VM --troubleshoot --zone=ZONE` | Add firewall rule, grant OS Login role, or use `--tunnel-through-iap`. |
| Startup script did not run | Script error, guest agent issue, metadata typo | `gcloud compute instances get-serial-port-output VM --zone=ZONE` | Check serial logs and metadata keys. |
| Cloud Run deploy succeeds but requests fail | Wrong port, failed container startup, missing env vars | `gcloud run services logs read SERVICE --region=REGION` | Ensure container listens on `$PORT`, set env vars/secrets. |
| GKE `kubectl` points to wrong cluster | kubeconfig context is stale | `kubectl config current-context`; `kubectl config get-contexts` | Rerun `gcloud container clusters get-credentials`. |
| Batch job stuck or failed | Quota, machine availability, image error, script failure | `gcloud batch jobs describe JOB --location=REGION`; Cloud Logging query | Adjust machine type/location, quota, image, or retry settings. |
| GPU VM does not show GPU | Driver not installed or wrong image | `nvidia-smi`; serial logs | Install NVIDIA driver or use supported GPU image. |

Use `--format` and `--filter` for repeatable automation:

```bash
# Get external IPs for running web instances.
gcloud compute instances list \
  --filter='status=RUNNING AND labels.app=web' \
  --format='table(name,zone.basename(),networkInterfaces[0].accessConfigs[0].natIP)'

# Return only a Cloud Run service URL.
gcloud run services describe "$SERVICE" \
  --region="$REGION" \
  --format='value(status.url)'
```

## 9. End-to-end quick starts

### 9.1 VM web server in 5 commands

```bash
export PROJECT_ID="my-gcp-project"
export REGION="us-central1"
export ZONE="us-central1-a"

gcloud config set project "$PROJECT_ID"
gcloud services enable compute.googleapis.com

gcloud compute instances create quick-web \
  --zone="$ZONE" \
  --machine-type=e2-micro \
  --image-family=debian-12 \
  --image-project=debian-cloud \
  --tags=http-server \
  --metadata=startup-script='#!/bin/bash
apt-get update
apt-get install -y nginx
systemctl enable --now nginx
'

gcloud compute firewall-rules create allow-quick-web \
  --network=default \
  --allow=tcp:80 \
  --target-tags=http-server

gcloud compute instances list --filter='name=quick-web'
```

### 9.2 Cloud Run container in 3 commands

```bash
export PROJECT_ID="my-gcp-project"
export REGION="us-central1"

gcloud config set project "$PROJECT_ID"
gcloud services enable run.googleapis.com artifactregistry.googleapis.com cloudbuild.googleapis.com

gcloud run deploy hello-run \
  --image=us-docker.pkg.dev/cloudrun/container/hello:latest \
  --region="$REGION" \
  --allow-unauthenticated
```

### 9.3 GKE Autopilot app in 5 commands

```bash
export PROJECT_ID="my-gcp-project"
export REGION="us-central1"
export CLUSTER="autopilot-cluster"

gcloud config set project "$PROJECT_ID"
gcloud services enable container.googleapis.com

gcloud container clusters create-auto "$CLUSTER" --location="$REGION"
gcloud container clusters get-credentials "$CLUSTER" --location="$REGION"

kubectl create deployment hello \
  --image=us-docker.pkg.dev/google-samples/containers/gke/hello-app:1.0
kubectl expose deployment hello --type=LoadBalancer --port=80 --target-port=8080
```

## 10. Practical decision tree

1. If the workload is a single stateless HTTP container, start with **Cloud Run service**.
2. If the workload is a run-to-completion container task, start with **Cloud Run Job**.
3. If the workload needs many parallel tasks, task arrays, Spot capacity, or HPC scheduling, start with **Google Cloud Batch**.
4. If the workload needs Kubernetes APIs, sidecars, service mesh, or multiple microservices, start with **GKE Autopilot** and move to Standard only when node-level control is required.
5. If the workload needs full OS control or legacy VM deployment patterns, start with **Compute Engine**, and use **MIGs** for scaling or high availability.
6. If the workload needs accelerators, choose **GPU VMs**, **Batch GPU jobs**, **GKE GPU nodes**, or **Cloud TPU VMs** based on framework compatibility, quota, availability, and operational model.
