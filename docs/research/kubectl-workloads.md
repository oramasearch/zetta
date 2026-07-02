# Research: kubectl Workloads Guide

> **Provenance.** Captured 2026-06-26 from a user-supplied hands-on guide
> ("kubectl Workloads Guide: Launch and Manage Applications on Kubernetes"), whose
> command syntax is grounded in the official Kubernetes / kubectl docs
> (primary-source links are inline below).
>
> **Status:** reference / background. It is the cited basis for the `kubectl`
> command shapes the **Kubernetes cloud-burst provisioner** uses —
> `K8sCliControlPlane` (`src/core/cloud/k8s_cli_control_plane.cpp`); see
> [k8s-provisioner.md](../cloud/k8s-provisioner.md). zetta's burst backend uses only the
> **Deployment + scale** slice (§3.2 Deployments, §5.1 manual scaling, §6.2
> nodeSelector node targeting); the rest (Pods, StatefulSets, DaemonSets, Jobs,
> CronJobs, Services, ConfigMaps/Secrets, Ingress, HPA, debugging) is background for
> choosing primitives and for the cloud equivalents in
> [cloud-provisioner-pattern.md](../cloud/cloud-provisioner-pattern.md). Unlike the cloud
> CLI guides, this assumes the cluster already exists (the kubeconfig context).
> Examples are illustrative — replace names, namespaces, images, and ports with
> values that match your environment.

---

# kubectl Workloads Guide: Launch and Manage Applications on Kubernetes

This guide is a hands-on, command-first walkthrough for using `kubectl` to deploy and operate workloads **on an existing Kubernetes cluster**. Unlike cloud CLI compute guides that create cloud infrastructure, this guide assumes the cluster already exists and focuses on what you do next: connect `kubectl`, create Kubernetes objects, expose applications, scale workloads, debug failures, and clean up. Kubernetes describes `kubectl` as the command-line tool for communicating with a cluster control plane through the Kubernetes API, and the tool normally reads configuration from `$HOME/.kube/config` unless you set `KUBECONFIG` or pass `--kubeconfig` ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/)).

> Replace names, namespaces, images, and ports with values that match your environment. Most examples use the `demo` namespace so you can delete the whole walkthrough safely at the end.

---

## 1. Getting started with `kubectl`

### 1.1 What `kubectl` does

`kubectl` sends API requests to the Kubernetes API server using credentials, cluster endpoints, and context information from kubeconfig files ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/)). Kubernetes itself is a platform for managing containerized workloads and services through declarative configuration and automation, so most `kubectl` work is either submitting desired state or inspecting the current state ([Kubernetes overview](https://kubernetes.io/docs/concepts/overview/)).

A practical mental model:

| Concept | Meaning for workload operators |
|---|---|
| Cluster | The Kubernetes environment where workloads run. Kubernetes runs containerized tasks across a cluster of nodes ([Kubernetes overview](https://kubernetes.io/docs/concepts/overview/)). |
| Node | A worker machine that can run Pods; `kubectl get nodes` shows the nodes registered with the cluster. |
| Namespace | A logical scope for names and resource isolation inside one cluster; resource names must be unique within a namespace but not across namespaces ([Kubernetes namespaces](https://kubernetes.io/docs/concepts/overview/working-with-objects/namespaces/)). |
| Pod | The smallest deployable unit in Kubernetes; a Pod contains one or more containers that share storage and network resources ([Kubernetes Pods](https://kubernetes.io/docs/concepts/workloads/pods/)). |
| Controller object | A higher-level object, such as a Deployment, StatefulSet, DaemonSet, Job, or CronJob, that creates and reconciles Pods for you ([Kubernetes workload management](https://kubernetes.io/docs/concepts/workloads/controllers/)). |

### 1.2 Install `kubectl` and check version compatibility

Install `kubectl` for your operating system from the official Kubernetes tooling page; Kubernetes documents Linux, macOS, and Windows installation paths ([Kubernetes install tools](https://kubernetes.io/docs/tasks/tools/)). `kubectl` should be within one minor version older or newer than the cluster API server, so a v1.36 API server supports `kubectl` v1.35, v1.36, or v1.37 ([Kubernetes version skew policy](https://kubernetes.io/releases/version-skew-policy/)).

```bash
kubectl version --client
kubectl version
```

Key flags and output:

- `kubectl version --client` checks only the local `kubectl` binary.
- `kubectl version` also contacts the cluster and is a useful first compatibility check.

### 1.3 Understand kubeconfig files, contexts, users, and namespaces

Kubeconfig files organize cluster endpoints, users, namespaces, and authentication details, and `kubectl` uses them to choose a cluster and communicate with the API server ([Kubernetes kubeconfig documentation](https://kubernetes.io/docs/concepts/configuration/organize-cluster-access-kubeconfig/)). By default, `kubectl` looks for a file named `config` in `$HOME/.kube`, but you can override that with the `KUBECONFIG` environment variable or `--kubeconfig` flag ([Kubernetes kubeconfig documentation](https://kubernetes.io/docs/concepts/configuration/organize-cluster-access-kubeconfig/)).

> Security note: only use kubeconfig files from trusted sources because a specially crafted kubeconfig file can expose files or execute malicious code ([Kubernetes kubeconfig documentation](https://kubernetes.io/docs/concepts/configuration/organize-cluster-access-kubeconfig/)).

```bash
# Show merged kubeconfig settings.
kubectl config view

# Show contexts in your kubeconfig.
kubectl config get-contexts

# Print the current context name.
kubectl config current-context

# Switch to another context.
kubectl config use-context my-cluster-context

# Set the default namespace for the current context.
kubectl config set-context --current --namespace=demo
```

Key flags and concepts:

- A **context** groups a cluster, user, and namespace under a friendly name ([Kubernetes kubeconfig documentation](https://kubernetes.io/docs/concepts/configuration/organize-cluster-access-kubeconfig/)).
- `kubectl config get-contexts` lists contexts, and `kubectl config use-context` changes the default context ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).
- `kubectl config set-context --current --namespace=demo` saves the namespace for future commands in the current context ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

### 1.4 Connect to a cluster

Cloud provider CLIs usually populate kubeconfig for managed Kubernetes clusters, after which regular `kubectl` commands operate on workloads inside the cluster. For AKS, `az aks get-credentials` gets access credentials and merges them into `.kube/config` by default ([Microsoft Learn az aks get-credentials](https://learn.microsoft.com/en-us/cli/azure/aks?view=azure-cli-latest#az-aks-get-credentials)). For EKS, `aws eks update-kubeconfig` configures `kubectl` for an Amazon EKS cluster and writes or merges kubeconfig at the default location unless overridden ([AWS CLI update-kubeconfig](https://docs.aws.amazon.com/cli/latest/reference/eks/update-kubeconfig.html)). For GKE, `gcloud container clusters get-credentials` fetches credentials for a running cluster ([Google Cloud SDK get-credentials](https://cloud.google.com/sdk/gcloud/reference/container/clusters/get-credentials)). For OCI OKE, `oci ce cluster create-kubeconfig` creates kubeconfig YAML for a cluster ([Oracle OCI CLI create-kubeconfig](https://docs.oracle.com/en-us/iaas/tools/oci-cli/latest/oci_cli_docs/cmdref/ce/cluster/create-kubeconfig.html)).

Examples:

```bash
# Azure AKS
az aks get-credentials \
  --resource-group my-rg \
  --name my-aks

# Amazon EKS
aws eks update-kubeconfig \
  --region us-west-2 \
  --name my-eks

# Google GKE
gcloud container clusters get-credentials my-gke \
  --location us-central1

# Oracle OKE
oci ce cluster create-kubeconfig \
  --cluster-id ocid1.cluster.oc1..example \
  --file "$HOME/.kube/config" \
  --region us-ashburn-1 \
  --token-version 2.0.0
```

### 1.5 Verify connectivity

The official quick reference lists `kubectl cluster-info`, `kubectl get nodes`, and `kubectl version` as common connectivity and discovery commands ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

```bash
kubectl cluster-info
kubectl get nodes
kubectl get nodes -o wide
kubectl version
```

What to look for:

- `kubectl cluster-info` prints the control plane and service endpoints known to the cluster.
- `kubectl get nodes` confirms that your credentials can list cluster-level Node objects.
- `kubectl get nodes -o wide` adds useful columns such as node IPs, OS image, container runtime, and kernel version.

Create a working namespace for the examples:

```bash
kubectl create namespace demo
kubectl config set-context --current --namespace=demo
kubectl get namespace demo
```

Namespaces isolate groups of resources in a single cluster, and production clusters commonly avoid putting application workloads in the `default` namespace ([Kubernetes namespaces](https://kubernetes.io/docs/concepts/overview/working-with-objects/namespaces/)).

---

## 2. Core `kubectl` mechanics

### 2.1 Imperative vs. declarative workflows

`kubectl` supports imperative commands that create or modify objects directly, such as `kubectl run`, `kubectl create deployment`, and `kubectl expose`, and declarative commands that apply manifest files with `kubectl apply -f` ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/)). Kubernetes recommends `kubectl apply` for managing applications in production because it creates and updates resources from files that define Kubernetes objects ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

Imperative style is useful for learning, testing, and generating starter YAML:

```bash
kubectl create deployment web --image=nginx:1.27 --replicas=2
kubectl expose deployment web --port=80 --target-port=80 --type=ClusterIP
```

Declarative style is useful for repeatable environments, reviewable changes, GitOps, CI/CD, and team operations:

```bash
kubectl apply -f deployment.yaml
kubectl apply -f service.yaml
kubectl apply -f ./manifests/
```

A good workflow is to generate YAML imperatively, edit it, then apply it declaratively:

```bash
kubectl create deployment web \
  --image=nginx:1.27 \
  --replicas=2 \
  --dry-run=client \
  -o yaml > deployment.yaml

kubectl apply -f deployment.yaml
```

The `--dry-run=client -o yaml` pattern is documented for generating an object spec without sending it to the API server ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

### 2.2 The `get`, `describe`, and `explain` trio

The core inspection loop is `kubectl get` for lists and summaries, `kubectl describe` for detailed state and events, and `kubectl explain` for API-field documentation ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/)).

```bash
# List resources.
kubectl get pods
kubectl get deploy,rs,pods,svc

# Add placement and IP details.
kubectl get pods -o wide

# Inspect one object in detail, including events.
kubectl describe pod my-pod
kubectl describe deployment web

# Read schema help for fields.
kubectl explain pods
kubectl explain deployment.spec.strategy
kubectl explain job.spec.template.spec.restartPolicy
```

Key flags:

- `-o wide` prints additional columns, and for Pods it includes the node name ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).
- `kubectl describe` is verbose and includes related events, which makes it useful for failed scheduling, image pull errors, readiness failures, and restarts ([Kubernetes troubleshooting applications](https://kubernetes.io/docs/tasks/debug/debug-application/)).
- `kubectl explain TYPE` reads API schema documentation for resources such as Pods, Nodes, and Services ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/)).

### 2.3 Output formats: wide, YAML, JSON, JSONPath, and names

`kubectl` supports `-o` / `--output` to print objects in different formats, including `wide`, `yaml`, `json`, `jsonpath`, and `name` ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

```bash
kubectl get pods -o wide
kubectl get pod my-pod -o yaml
kubectl get deployment web -o json
kubectl get pods -o name
kubectl get pods -o jsonpath='{.items[*].metadata.name}'
echo
```

Common use cases:

- `-o yaml` is best when you want to inspect full live configuration.
- `-o json` is best for automation that consumes Kubernetes API objects.
- `-o jsonpath='...'` extracts specific fields for scripts; the quick reference includes examples for labels and config data extraction ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).
- `-o name` prints resource identifiers such as `pod/web-abc123` for piping into other commands.

### 2.4 Labels, selectors, and namespaces

Labels are the glue between controllers, Services, and operational commands. `kubectl get ... -l` filters resources by label, and the quick reference shows `kubectl get pods -l name=myLabel` and deletion by label as common selector patterns ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

```bash
# Show labels on Pods.
kubectl get pods --show-labels

# Filter by label.
kubectl get pods -l app=web
kubectl get deploy,svc -l app=web

# Use a namespace explicitly.
kubectl get pods -n demo

# Search every namespace.
kubectl get pods --all-namespaces
kubectl get pods -A
```

Key flags:

- `-l app=web` selects resources whose labels include `app=web`.
- `-n demo` targets a namespace for this command only.
- `--all-namespaces` or `-A` searches across namespaces, where supported ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

### 2.5 Dry runs and YAML generation

Dry runs let you validate or generate definitions before creating anything. The quick reference documents generating YAML with `kubectl run nginx --image=nginx --dry-run=client -o yaml > pod.yaml` ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

```bash
# Generate a Pod manifest.
kubectl run web-pod \
  --image=nginx:1.27 \
  --restart=Never \
  --dry-run=client \
  -o yaml > pod.yaml

# Generate a Deployment manifest.
kubectl create deployment web \
  --image=nginx:1.27 \
  --replicas=2 \
  --dry-run=client \
  -o yaml > deployment.yaml

# Generate a Service manifest from an existing Deployment.
kubectl expose deployment web \
  --port=80 \
  --target-port=80 \
  --type=ClusterIP \
  --dry-run=client \
  -o yaml > service.yaml
```

Use `--dry-run=client` when you only need local object generation and `--dry-run=server` when you want the API server to validate against admission logic without persisting the object.

---

## 3. Running workloads: workload controllers

### 3.1 Pods: direct execution for learning and one-off tests

A Pod is the smallest deployable computing unit in Kubernetes, and it contains one or more containers with shared storage and network resources ([Kubernetes Pods](https://kubernetes.io/docs/concepts/workloads/pods/)). Pods are designed to be ephemeral and are generally created by workload resources such as Deployments or Jobs rather than managed directly ([Kubernetes Pods](https://kubernetes.io/docs/concepts/workloads/pods/)).

Create and inspect a simple Pod:

```bash
kubectl run nginx-pod \
  --image=nginx:1.27 \
  --restart=Never \
  --port=80

kubectl get pods
kubectl get pod nginx-pod -o wide
kubectl describe pod nginx-pod
```

Key flags:

- `--image=nginx:1.27` chooses the container image.
- `--restart=Never` makes `kubectl run` create a standalone Pod instead of a controller-backed object.
- `--port=80` records a container port in the Pod spec; it does not expose the Pod outside the cluster.

View logs:

```bash
kubectl logs nginx-pod
kubectl logs -f nginx-pod
kubectl logs nginx-pod --previous
```

The `logs` command prints container logs, `-f` follows the stream, and `--previous` retrieves logs from a previously terminated container instance ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

Run a command inside a container:

```bash
kubectl exec nginx-pod -- ls /
kubectl exec -it nginx-pod -- sh
```

The `exec` command runs a command inside a container, and `-it` is the usual interactive terminal combination for opening a shell ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

Forward a local port to a Pod:

```bash
kubectl port-forward pod/nginx-pod 8080:80
```

`kubectl port-forward` listens locally and forwards traffic to a Pod, Service, or Deployment target; for example, the quick reference shows forwarding local port 5000 to a Pod or Service ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

Clean up the standalone Pod:

```bash
kubectl delete pod nginx-pod
```

### 3.2 Deployments: stateless apps, rolling updates, and self-healing

A Deployment manages a set of Pods for an application workload, usually a stateless workload, and provides declarative updates for Pods and ReplicaSets ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)). You describe the desired state in the Deployment, and the Deployment controller changes actual state toward that desired state at a controlled rate ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)).

Imperative Deployment example:

```bash
kubectl create deployment web \
  --image=nginx:1.27 \
  --replicas=3

kubectl get deployments
kubectl get rs
kubectl get pods -l app=web -o wide
```

Scale manually:

```bash
kubectl scale deployment/web --replicas=5
kubectl get deployment web
kubectl get pods -l app=web
```

`kubectl scale TYPE/NAME --replicas=COUNT` updates the desired replica count for a scalable resource ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/)).

Update the image and watch the rollout:

```bash
kubectl set image deployment/web nginx=nginx:1.28
kubectl rollout status deployment/web
kubectl rollout history deployment/web
```

The Deployment documentation shows `kubectl set image deployment/nginx-deployment nginx=nginx:1.16.1`, `kubectl rollout status`, and `kubectl rollout history` for updating and inspecting rollouts ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)). A Deployment rollout is triggered when the Pod template changes, such as updating labels or container images ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)).

Undo a rollout:

```bash
kubectl rollout undo deployment/web
kubectl rollout status deployment/web

# Or roll back to a specific revision.
kubectl rollout history deployment/web
kubectl rollout undo deployment/web --to-revision=2
```

The Deployment controller creates new ReplicaSets for updated Pod templates and can roll back to earlier revisions when the current rollout is unstable ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)).

### 3.3 Deployment + Service: imperative and declarative side by side

Imperative version:

```bash
kubectl create deployment web \
  --image=nginx:1.27 \
  --replicas=3

kubectl expose deployment web \
  --name=web \
  --port=80 \
  --target-port=80 \
  --type=ClusterIP

kubectl get deploy,svc,pods -l app=web
```

Declarative version saved as `web.yaml`:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: web
  labels:
    app: web
spec:
  replicas: 3
  selector:
    matchLabels:
      app: web
  template:
    metadata:
      labels:
        app: web
    spec:
      containers:
        - name: nginx
          image: nginx:1.27
          ports:
            - containerPort: 80
          resources:
            requests:
              cpu: 100m
              memory: 128Mi
            limits:
              cpu: 500m
              memory: 256Mi
          readinessProbe:
            httpGet:
              path: /
              port: 80
            initialDelaySeconds: 5
            periodSeconds: 10
          livenessProbe:
            httpGet:
              path: /
              port: 80
            initialDelaySeconds: 15
            periodSeconds: 20
---
apiVersion: v1
kind: Service
metadata:
  name: web
  labels:
    app: web
spec:
  type: ClusterIP
  selector:
    app: web
  ports:
    - name: http
      port: 80
      targetPort: 80
```

Apply and test:

```bash
kubectl apply -f web.yaml
kubectl rollout status deployment/web
kubectl get svc web
kubectl port-forward svc/web 8080:80
```

In a Deployment manifest, `.spec.selector` must match `.spec.template.metadata.labels`, and the Pod template is nested under `.spec.template` ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)). A Service usually targets Pods using a selector and creates a stable network abstraction over an ephemeral set of Pods ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/)).

### 3.4 ReplicaSets: know they exist, rarely manage them directly

A Deployment indirectly manages ReplicaSets, and the Deployment documentation specifically warns not to manage ReplicaSets owned by a Deployment ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)). In day-to-day operations, use ReplicaSet visibility for rollout diagnosis rather than direct changes.

```bash
kubectl get rs
kubectl describe rs -l app=web
kubectl get pods --show-labels
```

The ReplicaSet name usually includes the Deployment name plus a hash, and the Deployment uses ReplicaSets to bring up new Pods while scaling down old ones during rollouts ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)).

### 3.5 StatefulSets: stable identity and storage for stateful apps

A StatefulSet manages Pods that need persistent storage or stable, unique network identity, and each Pod keeps a sticky identity across rescheduling ([Kubernetes StatefulSets](https://kubernetes.io/docs/concepts/workloads/controllers/statefulset/)). StatefulSets are useful when Pods are not interchangeable, such as database members, clustered services, or workloads tied to persistent volumes ([Kubernetes StatefulSets](https://kubernetes.io/docs/concepts/workloads/controllers/statefulset/)).

Common commands:

```bash
kubectl get statefulsets
kubectl describe statefulset my-db
kubectl scale statefulset/my-db --replicas=3
kubectl rollout status statefulset/my-db
kubectl rollout history statefulset/my-db
```

Important behavior:

- StatefulSet Pods are created with stable ordinal names such as `web-0`, `web-1`, and `web-2` ([Kubernetes StatefulSets](https://kubernetes.io/docs/concepts/workloads/controllers/statefulset/)).
- StatefulSet rollout and scaling can be ordered, so operations may wait for predecessors to become Running and Ready before proceeding ([Kubernetes StatefulSets](https://kubernetes.io/docs/concepts/workloads/controllers/statefulset/)).
- PersistentVolumeClaims created from `volumeClaimTemplates` are not automatically deleted when Pods or the StatefulSet are deleted ([Kubernetes StatefulSets](https://kubernetes.io/docs/concepts/workloads/controllers/statefulset/)).

### 3.6 DaemonSets: one Pod per node or per selected node

A DaemonSet ensures that all or some eligible Nodes run a copy of a Pod, and Kubernetes adds Pods as nodes join and garbage-collects them as nodes leave ([Kubernetes DaemonSet](https://kubernetes.io/docs/concepts/workloads/controllers/daemonset/)). DaemonSets are common for node-local agents such as log collectors, monitoring agents, storage daemons, networking helpers, and GPU/device plugins ([Kubernetes DaemonSet](https://kubernetes.io/docs/concepts/workloads/controllers/daemonset/)).

Common commands:

```bash
kubectl get daemonsets -A
kubectl describe daemonset my-agent -n kube-system
kubectl rollout status daemonset/my-agent -n kube-system
kubectl get pods -n kube-system -o wide -l app=my-agent
```

Target only some nodes by using `nodeSelector` or affinity in the Pod template, because the DaemonSet controller creates Pods only on nodes that match those constraints ([Kubernetes DaemonSet](https://kubernetes.io/docs/concepts/workloads/controllers/daemonset/)).

### 3.7 Jobs: one-off and batch workloads

A Job represents a one-off task that runs to completion, creates one or more Pods, and retries Pods until the configured completions succeed ([Kubernetes Jobs](https://kubernetes.io/docs/concepts/workloads/controllers/job/)). Jobs are the Kubernetes equivalent of batch compute for tasks such as data processing, model training steps, migrations, and report generation.

Imperative Job example:

```bash
kubectl create job pi \
  --image=perl:5.34 \
  -- perl -Mbignum=bpi -wle 'print bpi(2000)'

kubectl get jobs
kubectl get pods -l batch.kubernetes.io/job-name=pi
kubectl logs job/pi
kubectl delete job pi
```

The official Job documentation shows listing Pods for a Job with the `batch.kubernetes.io/job-name` selector and reading output with `kubectl logs jobs/pi` ([Kubernetes Jobs](https://kubernetes.io/docs/concepts/workloads/controllers/job/)).

Declarative Job example saved as `pi-job.yaml`:

```yaml
apiVersion: batch/v1
kind: Job
metadata:
  name: pi
  labels:
    app: pi
spec:
  backoffLimit: 4
  template:
    metadata:
      labels:
        app: pi
    spec:
      restartPolicy: Never
      containers:
        - name: pi
          image: perl:5.34
          command: ["perl", "-Mbignum=bpi", "-wle", "print bpi(2000)"]
          resources:
            requests:
              cpu: 250m
              memory: 256Mi
            limits:
              cpu: "1"
              memory: 512Mi
```

Apply and inspect:

```bash
kubectl apply -f pi-job.yaml
kubectl wait --for=condition=complete job/pi --timeout=120s
kubectl logs job/pi
kubectl delete -f pi-job.yaml
```

### 3.8 CronJobs: scheduled batch workloads

A CronJob starts one-time Jobs on a repeating schedule, similar to one line in a Unix crontab file ([Kubernetes CronJob](https://kubernetes.io/docs/concepts/workloads/controllers/cron-jobs/)). CronJobs are suitable for recurring backups, report generation, cleanup jobs, and scheduled data pipelines ([Kubernetes CronJob](https://kubernetes.io/docs/concepts/workloads/controllers/cron-jobs/)).

Imperative CronJob example:

```bash
kubectl create cronjob hello \
  --image=busybox:1.28 \
  --schedule='*/5 * * * *' \
  -- /bin/sh -c 'date; echo Hello from Kubernetes'

kubectl get cronjobs
kubectl get jobs
kubectl delete cronjob hello
```

The quick reference includes `kubectl create cronjob hello --image=busybox:1.28 --schedule="*/1 * * * *" -- echo "Hello World"` as a CronJob creation example ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). CronJob schedules use five fields for minute, hour, day of month, month, and day of week, and macros such as `@hourly`, `@daily`, and `@weekly` are supported ([Kubernetes CronJob](https://kubernetes.io/docs/concepts/workloads/controllers/cron-jobs/)).

---

## 4. Configuration and networking for workloads

### 4.1 Services: stable networking for Pods

A Service exposes a network application running as one or more Pods and provides a stable abstraction over Pods that may be created, deleted, or replaced ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/)). The Service target set is usually determined by a selector, such as `app=web` ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/)).

Create a Service from a Deployment:

```bash
kubectl expose deployment web \
  --name=web \
  --port=80 \
  --target-port=80 \
  --type=ClusterIP

kubectl get svc
kubectl describe svc web
```

Service types:

| Type | Use it when | Behavior |
|---|---|---|
| `ClusterIP` | Internal-only service discovery | Exposes the Service on a cluster-internal IP and is the default type ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/)). |
| `NodePort` | Simple external access through node IPs | Exposes the Service on each node at a static port, backed by a ClusterIP Service ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/)). |
| `LoadBalancer` | Managed external load balancing | Exposes the Service through an external load balancer when the cluster integrates with a provider that supports it ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/)). |

Examples:

```bash
# Internal Service.
kubectl expose deployment web --port=80 --target-port=80 --type=ClusterIP

# NodePort Service.
kubectl expose deployment web --port=80 --target-port=80 --type=NodePort --name=web-nodeport
kubectl get svc web-nodeport

# Cloud load balancer Service, if supported by the cluster.
kubectl expose deployment web --port=80 --target-port=80 --type=LoadBalancer --name=web-lb
kubectl get svc web-lb --watch
```

`LoadBalancer` Service behavior depends on the cluster’s cloud-controller or load balancer integration; Kubernetes itself does not directly provide the external load balancer component ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/)).

### 4.2 ConfigMaps: non-secret configuration

A ConfigMap stores non-confidential key-value configuration, and Pods can consume ConfigMaps as environment variables, command-line arguments, or files in a volume ([Kubernetes ConfigMaps](https://kubernetes.io/docs/concepts/configuration/configmap/)). ConfigMaps decouple environment-specific configuration from container images, but they do not provide secrecy or encryption ([Kubernetes ConfigMaps](https://kubernetes.io/docs/concepts/configuration/configmap/)).

Create ConfigMaps imperatively:

```bash
kubectl create configmap app-config \
  --from-literal=APP_MODE=production \
  --from-literal=LOG_LEVEL=info

kubectl create configmap nginx-config \
  --from-file=nginx.conf=./nginx.conf

kubectl get configmap app-config -o yaml
```

Inject a ConfigMap into a Deployment as environment variables:

```bash
kubectl set env deployment/web \
  --from=configmap/app-config

kubectl rollout status deployment/web
```

Manifest example for environment variables:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: config-demo
spec:
  replicas: 1
  selector:
    matchLabels:
      app: config-demo
  template:
    metadata:
      labels:
        app: config-demo
    spec:
      containers:
        - name: app
          image: busybox:1.36
          command: ["sh", "-c", "env | sort; sleep 3600"]
          envFrom:
            - configMapRef:
                name: app-config
```

When a ConfigMap is mounted as a volume, each key becomes a file under the mount path; when consumed as environment variables, ConfigMap updates require a Pod restart to appear in the container environment ([Kubernetes ConfigMaps](https://kubernetes.io/docs/concepts/configuration/configmap/)).

### 4.3 Secrets: sensitive configuration

A Secret stores a small amount of sensitive data such as a password, token, or key, and Secrets can be mounted as volumes or exposed as environment variables to containers ([Kubernetes Secrets](https://kubernetes.io/docs/concepts/configuration/secret/)). Kubernetes warns that Secrets are stored unencrypted in `etcd` by default, so production clusters should use encryption at rest and least-privilege RBAC for Secret access ([Kubernetes Secrets](https://kubernetes.io/docs/concepts/configuration/secret/)).

Create Secrets imperatively:

```bash
kubectl create secret generic db-creds \
  --from-literal=username=appuser \
  --from-literal=password='change-me'

kubectl get secret db-creds
kubectl get secret db-creds -o yaml
```

Inject a Secret into a container as environment variables:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: secret-demo
spec:
  replicas: 1
  selector:
    matchLabels:
      app: secret-demo
  template:
    metadata:
      labels:
        app: secret-demo
    spec:
      containers:
        - name: app
          image: busybox:1.36
          command: ["sh", "-c", "echo DB user is $DB_USERNAME; sleep 3600"]
          env:
            - name: DB_USERNAME
              valueFrom:
                secretKeyRef:
                  name: db-creds
                  key: username
            - name: DB_PASSWORD
              valueFrom:
                secretKeyRef:
                  name: db-creds
                  key: password
```

Use `stringData` in hand-written manifests when you want to provide clear text that the API server stores in the Secret’s `data` field, but avoid committing real secrets to source control. Kubernetes notes that `stringData` is internally merged into `data` and takes precedence for duplicate keys ([Kubernetes Secrets](https://kubernetes.io/docs/concepts/configuration/secret/)).

### 4.4 Ingress: HTTP routing into Services

Ingress manages external HTTP and HTTPS access to Services in a cluster through rules such as host and path matches ([Kubernetes Ingress](https://kubernetes.io/docs/concepts/services-networking/ingress/)). Creating an Ingress object alone has no effect unless an Ingress controller is installed to satisfy it ([Kubernetes Ingress](https://kubernetes.io/docs/concepts/services-networking/ingress/)).

Minimal example:

```yaml
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: web
spec:
  ingressClassName: nginx
  rules:
    - host: web.example.com
      http:
        paths:
          - path: /
            pathType: Prefix
            backend:
              service:
                name: web
                port:
                  number: 80
```

Ingress is for HTTP and HTTPS routing, while non-HTTP protocols typically use `NodePort` or `LoadBalancer` Services ([Kubernetes Ingress](https://kubernetes.io/docs/concepts/services-networking/ingress/)).

### 4.5 Resource requests and limits

Resource requests tell the scheduler how much CPU and memory a container needs, and resource limits tell the kubelet and container runtime how much the container is allowed to use ([Kubernetes resource management](https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/)). The scheduler places Pods based on requests, even if actual usage is currently low, to protect nodes from future resource pressure ([Kubernetes resource management](https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/)).

Example:

```yaml
resources:
  requests:
    cpu: 250m
    memory: 256Mi
  limits:
    cpu: "1"
    memory: 512Mi
```

Practical guidance:

- Use CPU millicores such as `250m`; Kubernetes treats `100m` as one tenth of a CPU ([Kubernetes resource management](https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/)).
- Use memory units such as `Mi` or `Gi`; be careful because lowercase `m` means millibytes, not megabytes ([Kubernetes resource management](https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/)).
- Memory limits are enforced reactively with out-of-memory kills, while CPU limits are enforced through CPU throttling ([Kubernetes resource management](https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/)).

### 4.6 Namespaces and ResourceQuotas

Namespaces are a common way to divide cluster resources between teams, environments, or projects, and they provide the scope in which names are unique ([Kubernetes namespaces](https://kubernetes.io/docs/concepts/overview/working-with-objects/namespaces/)). A ResourceQuota limits aggregate resource consumption per namespace and can restrict object counts, compute requests and limits, and storage consumption ([Kubernetes ResourceQuotas](https://kubernetes.io/docs/concepts/policy/resource-quotas/)).

Example namespace and quota:

```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: team-a
---
apiVersion: v1
kind: ResourceQuota
metadata:
  name: team-a-quota
  namespace: team-a
spec:
  hard:
    requests.cpu: "4"
    requests.memory: 8Gi
    limits.cpu: "8"
    limits.memory: 16Gi
    pods: "20"
    services: "10"
```

Apply and inspect:

```bash
kubectl apply -f team-a-quota.yaml
kubectl describe resourcequota team-a-quota -n team-a
```

If CPU or memory quotas are enforced in a namespace, users must specify requests or limits for those resources on new Pods, or admission may reject the Pod ([Kubernetes ResourceQuotas](https://kubernetes.io/docs/concepts/policy/resource-quotas/)).

---

## 5. Scaling and autoscaling

### 5.1 Manual scaling

Manual scaling changes the desired replica count directly:

```bash
kubectl scale deployment/web --replicas=3
kubectl get deployment web
kubectl get pods -l app=web
```

The `kubectl scale` syntax is `kubectl scale ... --replicas=COUNT`, and the Deployment documentation shows scaling a Deployment with `kubectl scale deployment/nginx-deployment --replicas=10` ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/), [Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)).

### 5.2 Horizontal Pod Autoscaler

A HorizontalPodAutoscaler automatically updates a workload resource such as a Deployment or StatefulSet to match demand by changing the number of Pods ([Kubernetes horizontal pod autoscaling](https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/)). HPA commonly uses metrics APIs such as `metrics.k8s.io`, which is generally provided by `metrics-server` ([Kubernetes horizontal pod autoscaling](https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/)).

Create an HPA for a Deployment:

```bash
kubectl autoscale deployment web \
  --min=2 \
  --max=10 \
  --cpu-percent=70

kubectl get hpa
kubectl describe hpa web
```

Key flags:

- `--min=2` sets the lower replica bound.
- `--max=10` sets the upper replica bound.
- `--cpu-percent=70` targets average CPU utilization of 70% across Pods with CPU requests.

HPA CPU utilization is computed as current CPU usage divided by requested CPU, so Pods need CPU requests for CPU-based autoscaling to work ([Kubernetes horizontal pod autoscaling](https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/)). If an HPA manages a Deployment, avoid setting `.spec.replicas` in a repeatedly applied manifest unless you intend to override the autoscaler’s changes ([Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)).

Delete the HPA:

```bash
kubectl delete hpa web
```

### 5.3 Cluster Autoscaler and node-level autoscaling

Workload autoscaling changes Pod counts, while node autoscaling provisions or consolidates Nodes so Pods can schedule and infrastructure cost stays efficient ([Kubernetes node autoscaling](https://kubernetes.io/docs/concepts/cluster-administration/node-autoscaling/)). Kubernetes node autoscalers may add nodes when Pods cannot schedule on existing nodes and may remove underutilized nodes during consolidation ([Kubernetes node autoscaling](https://kubernetes.io/docs/concepts/cluster-administration/node-autoscaling/)).

Practical implication:

```bash
# If Pods are Pending, check scheduling events first.
kubectl get pods
kubectl describe pod <pending-pod-name>
kubectl get events --sort-by=.metadata.creationTimestamp
```

Cluster/node autoscaling is usually installed and managed by the cluster or cloud provider because it creates and deletes infrastructure resources such as virtual machines ([Kubernetes node autoscaling](https://kubernetes.io/docs/concepts/cluster-administration/node-autoscaling/)).

---

## 6. GPU, ML, and HPC workloads on Kubernetes

### 6.1 Request GPUs in Pod specs

Kubernetes treats GPUs as extended resources exposed by a vendor device plugin, and Pods consume them through resource names such as `nvidia.com/gpu` ([Kubernetes GPU scheduling](https://kubernetes.io/docs/tasks/manage-gpus/scheduling-gpus/)). GPUs should be specified in `resources.limits`, and if both requests and limits are present, the GPU request and limit values must be equal ([Kubernetes GPU scheduling](https://kubernetes.io/docs/tasks/manage-gpus/scheduling-gpus/)).

Example GPU Pod fragment:

```yaml
resources:
  limits:
    nvidia.com/gpu: 1
```

Complete GPU Job example:

```yaml
apiVersion: batch/v1
kind: Job
metadata:
  name: gpu-smoke-test
  labels:
    app: gpu-smoke-test
spec:
  backoffLimit: 1
  template:
    metadata:
      labels:
        app: gpu-smoke-test
    spec:
      restartPolicy: Never
      containers:
        - name: cuda
          image: nvidia/cuda:12.4.1-base-ubuntu22.04
          command: ["bash", "-lc", "nvidia-smi && sleep 10"]
          resources:
            limits:
              nvidia.com/gpu: 1
```

Run it:

```bash
kubectl apply -f gpu-job.yaml
kubectl get pods -l app=gpu-smoke-test -o wide
kubectl logs job/gpu-smoke-test
kubectl delete -f gpu-job.yaml
```

### 6.2 Device plugins and GPU node targeting

Kubernetes device plugins advertise vendor-specific hardware resources such as GPUs, NICs, FPGAs, and InfiniBand adapters to the kubelet, and vendors can deploy plugins manually or as DaemonSets ([Kubernetes device plugins](https://kubernetes.io/docs/concepts/extend-kubernetes/compute-storage-net/device-plugins/)). The NVIDIA GPU device plugin is the common plugin that exposes NVIDIA GPUs as `nvidia.com/gpu` resources ([Kubernetes device plugins](https://kubernetes.io/docs/concepts/extend-kubernetes/compute-storage-net/device-plugins/)).

Check GPU capacity:

```bash
kubectl get nodes -o jsonpath='{range .items[*]}{.metadata.name}{"\t"}{.status.allocatable.nvidia\.com/gpu}{"\n"}{end}'
```

Target GPU nodes with labels and selectors when needed:

```bash
# Label a node after verifying it is a GPU node.
kubectl label node gpu-node-1 accelerator=nvidia

# Use a nodeSelector in the Pod template.
cat > gpu-targeted-job.yaml <<'EOF'
apiVersion: batch/v1
kind: Job
metadata:
  name: gpu-targeted
spec:
  template:
    spec:
      restartPolicy: Never
      nodeSelector:
        accelerator: nvidia
      containers:
        - name: cuda
          image: nvidia/cuda:12.4.1-base-ubuntu22.04
          command: ["bash", "-lc", "nvidia-smi"]
          resources:
            limits:
              nvidia.com/gpu: 1
EOF
kubectl apply -f gpu-targeted-job.yaml
```

`nodeSelector` is the simplest node selection constraint, and Kubernetes schedules a Pod only onto nodes with all labels specified in the Pod’s `nodeSelector` ([Kubernetes assigning Pods to nodes](https://kubernetes.io/docs/concepts/scheduling-eviction/assign-pod-node/)).

Use taints and tolerations to keep non-GPU workloads away from GPU nodes:

```bash
kubectl taint nodes gpu-node-1 nvidia.com/gpu=true:NoSchedule
```

Taints repel Pods from nodes, while tolerations allow Pods with matching tolerations to schedule there without guaranteeing placement ([Kubernetes taints and tolerations](https://kubernetes.io/docs/concepts/scheduling-eviction/taint-and-toleration/)). Kubernetes recommends representing special hardware with extended resources, tainting the special hardware nodes, and using the `ExtendedResourceToleration` admission controller where available ([Kubernetes taints and tolerations](https://kubernetes.io/docs/concepts/scheduling-eviction/taint-and-toleration/)).

### 6.3 ML and HPC patterns

Use Jobs for one-off training runs, batch feature generation, simulations, and HPC tasks because a Job retries Pods until the configured completions succeed ([Kubernetes Jobs](https://kubernetes.io/docs/concepts/workloads/controllers/job/)). For distributed or array-style training, use parallel Jobs, framework-specific operators, or workload managers that create Kubernetes Jobs or Pods underneath.

Useful commands:

```bash
kubectl get jobs
kubectl describe job training-run
kubectl get pods -l batch.kubernetes.io/job-name=training-run -o wide
kubectl logs job/training-run
kubectl delete job training-run
```

---

## 7. Declarative workflow and best practices

### 7.1 Manage manifests with `kubectl apply`

Declarative object management stores object configuration files and uses `kubectl apply` to create and update the corresponding objects ([Kubernetes declarative object management](https://kubernetes.io/docs/tasks/manage-kubernetes-objects/declarative-config/)). `kubectl diff` previews the changes that `kubectl apply` would make ([Kubernetes declarative object management](https://kubernetes.io/docs/tasks/manage-kubernetes-objects/declarative-config/)).

```bash
# Preview changes.
kubectl diff -f ./manifests/

# Apply one file, multiple files, or a directory.
kubectl apply -f web.yaml
kubectl apply -f deployment.yaml -f service.yaml
kubectl apply -f ./manifests/

# Recursively apply directories.
kubectl apply -f ./manifests/ -R

# Inspect live state from the file reference.
kubectl get -f web.yaml -o yaml
```

Be consistent: Kubernetes documentation warns that mixing `kubectl apply` with imperative object configuration commands such as `create` and `replace` is not supported for the same objects because `apply` relies on a last-applied-configuration annotation ([Kubernetes declarative object management](https://kubernetes.io/docs/tasks/manage-kubernetes-objects/declarative-config/)).

### 7.2 Use Kustomize overlays with `kubectl apply -k`

Since Kubernetes v1.14, `kubectl` can manage objects using a `kustomization.yaml` file, and `kubectl apply -k <directory>` applies the rendered resources ([Kubernetes Kustomize](https://kubernetes.io/docs/tasks/manage-kubernetes-objects/kustomization/)). Kustomize bases and overlays let you share common manifests and customize names, labels, images, patches, ConfigMaps, and Secrets per environment ([Kubernetes Kustomize](https://kubernetes.io/docs/tasks/manage-kubernetes-objects/kustomization/)).

```bash
# Render without applying.
kubectl kustomize ./overlays/dev

# Preview differences.
kubectl diff -k ./overlays/dev

# Apply an overlay.
kubectl apply -k ./overlays/dev

# Delete an overlay's resources.
kubectl delete -k ./overlays/dev
```

### 7.3 Labels and annotations

Use labels for selection and grouping, and use annotations for non-identifying metadata such as descriptions, owner references, documentation URLs, or rollout notes. Services usually select Pods by label, and Deployments require selectors that match Pod template labels ([Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/), [Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/)).

```bash
kubectl label deployment web tier=frontend owner=platform
kubectl annotate deployment web description='Public web frontend'
kubectl get deploy -l tier=frontend
```

### 7.4 Probes for reliability

Liveness probes detect containers that are broken and need a restart, readiness probes indicate whether a Pod should receive Service traffic, and startup probes protect slow-starting containers from premature liveness failures ([Kubernetes probe configuration](https://kubernetes.io/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/)).

Example probe fragment:

```yaml
readinessProbe:
  httpGet:
    path: /ready
    port: 8080
  initialDelaySeconds: 5
  periodSeconds: 10
livenessProbe:
  httpGet:
    path: /healthz
    port: 8080
  initialDelaySeconds: 30
  periodSeconds: 20
startupProbe:
  httpGet:
    path: /healthz
    port: 8080
  failureThreshold: 30
  periodSeconds: 10
```

If a readiness probe fails, the Pod is marked unready and does not receive traffic from Services; if a liveness probe fails, Kubernetes kills and restarts the container ([Kubernetes probe configuration](https://kubernetes.io/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/)).

### 7.5 Node maintenance with cordon and drain

Use `cordon` to mark a node unschedulable, `drain` to evict workload Pods before maintenance, and `uncordon` to allow scheduling again; the official quick reference lists these commands for node maintenance ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

```bash
# Stop new Pods from scheduling on the node.
kubectl cordon worker-1

# Evict workload Pods so the node can be maintained.
kubectl drain worker-1 --ignore-daemonsets --delete-emptydir-data

# Return the node to service.
kubectl uncordon worker-1
```

Key flags:

- `--ignore-daemonsets` is commonly needed because DaemonSet Pods are managed by DaemonSets and are expected to run on eligible nodes.
- `--delete-emptydir-data` acknowledges that Pods using `emptyDir` volumes may lose local ephemeral data when evicted.

### 7.6 Debugging workflow

Start with status, events, logs, and object descriptions before changing anything. Kubernetes troubleshooting guidance uses `kubectl describe pod`, `kubectl logs`, `kubectl get events`, and `kubectl exec` as core application debugging commands ([Kubernetes troubleshooting applications](https://kubernetes.io/docs/tasks/debug/debug-application/)).

```bash
# 1. Find the failing object.
kubectl get pods -o wide
kubectl get deploy,rs,pods,svc

# 2. Inspect Pod status and events.
kubectl describe pod <pod-name>

# 3. Check recent events, sorted by time.
kubectl get events --sort-by=.metadata.creationTimestamp
kubectl events --types=Warning

# 4. Read logs.
kubectl logs <pod-name>
kubectl logs <pod-name> -c <container-name>
kubectl logs <pod-name> --previous
kubectl logs deploy/web

# 5. Exec into a running container.
kubectl exec -it <pod-name> -- sh
```

For images without a shell or for deeper runtime inspection, use ephemeral debug containers:

```bash
kubectl debug -it <pod-name> \
  --image=busybox:1.28 \
  --target=<container-name>

# Create a debuggable copy with a different image.
kubectl debug <pod-name> \
  -it \
  --copy-to=<pod-name>-debug \
  --set-image=*=ubuntu \
  --share-processes
```

The Kubernetes debugging guide shows `kubectl debug -it ephemeral-demo --image=busybox:1.28 --target=ephemeral-demo` for adding an ephemeral debugging container and `kubectl debug myapp -it --image=ubuntu --share-processes --copy-to=myapp-debug` for copying a Pod into a debuggable version ([Kubernetes debug running Pods](https://kubernetes.io/docs/tasks/debug/debug-application/debug-running-pod/)).

Common failure patterns:

| Symptom | First commands | Likely area |
|---|---|---|
| Pod stuck `Pending` | `kubectl describe pod <pod>`; `kubectl get events` | Scheduling, requests, quotas, node selectors, taints |
| `ImagePullBackOff` | `kubectl describe pod <pod>` | Image name, tag, registry auth, imagePullSecrets |
| `CrashLoopBackOff` | `kubectl logs <pod> --previous`; `kubectl describe pod <pod>` | App crash, command args, missing config, OOMKilled |
| Service has no traffic | `kubectl get endpointslice`; `kubectl get pods --show-labels`; `kubectl describe svc <svc>` | Selector mismatch, readiness failure, wrong targetPort |
| Rollout stuck | `kubectl rollout status deployment/<name>`; `kubectl describe deployment <name>` | Readiness failures, quota, image pull, insufficient resources |

### 7.7 Security and RBAC basics

Use `kubectl auth can-i` to check whether the current user can perform an action, because it queries the Kubernetes authorization layer through `SelfSubjectAccessReview` ([Kubernetes authorization](https://kubernetes.io/docs/reference/access-authn-authz/authorization/)). Administrators can also impersonate users or ServiceAccounts with `--as` to test access decisions ([Kubernetes authorization](https://kubernetes.io/docs/reference/access-authn-authz/authorization/)).

```bash
# Can I create Deployments in this namespace?
kubectl auth can-i create deployments --namespace demo

# Can I list Secrets?
kubectl auth can-i list secrets --namespace demo

# What can this ServiceAccount do in another namespace?
kubectl auth can-i list pods \
  --namespace target \
  --as system:serviceaccount:demo:default
```

Operational guidance:

- Prefer least-privilege Roles and RoleBindings per namespace.
- Avoid broad Secret access because anyone who can read Secrets in a namespace can retrieve sensitive data stored there ([Kubernetes Secrets](https://kubernetes.io/docs/concepts/configuration/secret/)).
- Be careful with kubeconfig files from untrusted sources because they can be unsafe ([Kubernetes kubeconfig documentation](https://kubernetes.io/docs/concepts/configuration/organize-cluster-access-kubeconfig/)).

### 7.8 Cleanup patterns

Delete the same manifests you applied:

```bash
kubectl delete -f web.yaml
kubectl delete -f ./manifests/
kubectl delete -k ./overlays/dev
```

Delete by label:

```bash
kubectl delete deploy,svc -l app=web
kubectl delete pods,services -l name=myLabel
```

Delete a namespace and everything in it:

```bash
kubectl delete namespace demo
```

The quick reference documents deletion from files, deletion by label, and deleting all Pods and Services in a namespace ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). Namespace deletion is convenient for labs, but use it carefully in shared clusters because it removes all namespaced resources in that namespace.

---

## 8. End-to-end mini lab

This short lab deploys a web app, exposes it, scales it, rolls it out, debugs it, and cleans it up.

```bash
# Create and select an isolated namespace.
kubectl create namespace demo
kubectl config set-context --current --namespace=demo

# Create a Deployment.
kubectl create deployment web --image=nginx:1.27 --replicas=2
kubectl rollout status deployment/web

# Expose it internally.
kubectl expose deployment web --port=80 --target-port=80 --type=ClusterIP
kubectl get deploy,rs,pods,svc -l app=web

# Test it locally.
kubectl port-forward svc/web 8080:80
# In another terminal: curl http://127.0.0.1:8080

# Scale it.
kubectl scale deployment/web --replicas=4
kubectl get pods -l app=web -o wide

# Update it.
kubectl set image deployment/web nginx=nginx:1.28
kubectl rollout status deployment/web
kubectl rollout history deployment/web

# Debug and observe.
kubectl describe deployment web
kubectl logs deploy/web
kubectl get events --sort-by=.metadata.creationTimestamp

# Clean up.
kubectl delete namespace demo
```

This lab uses the same mechanics documented across the official `kubectl` reference, quick reference, Deployment, Service, and debugging guides ([Kubernetes kubectl reference](https://kubernetes.io/docs/reference/kubectl/), [Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/), [Kubernetes Deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/), [Kubernetes Service](https://kubernetes.io/docs/concepts/services-networking/service/), [Kubernetes troubleshooting applications](https://kubernetes.io/docs/tasks/debug/debug-application/)).

---

## 9. Quick-reference command table

| Task | Command | Notes |
|---|---|---|
| Show client and server versions | `kubectl version` | Checks cluster connectivity as well as local client version. |
| Show cluster endpoints | `kubectl cluster-info` | Common connectivity check ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| List nodes | `kubectl get nodes -o wide` | Confirms cluster-level access and node status. |
| List contexts | `kubectl config get-contexts` | Shows available kubeconfig contexts ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Switch context | `kubectl config use-context <context>` | Changes the active cluster/user/namespace tuple. |
| Set default namespace | `kubectl config set-context --current --namespace=<ns>` | Saves namespace for future commands in the current context ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Create namespace | `kubectl create namespace <ns>` | Use namespaces for team or environment isolation. |
| Create Pod | `kubectl run <name> --image=<image> --restart=Never` | Best for tests; prefer controllers for real workloads. |
| Create Deployment | `kubectl create deployment <name> --image=<image> --replicas=<n>` | Starts a controller-managed stateless workload. |
| Expose Deployment | `kubectl expose deployment <name> --port=80 --target-port=8080 --type=ClusterIP` | Creates a Service for Pods behind the Deployment. |
| List common workload objects | `kubectl get deploy,rs,pods,svc` | Quick app overview. |
| Watch Pods | `kubectl get pods -w` | Streams status changes. |
| Show wide Pod placement | `kubectl get pods -o wide` | Includes node placement ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Filter by label | `kubectl get pods -l app=web` | Selects objects by labels. |
| All namespaces | `kubectl get pods -A` | Short form for `--all-namespaces` ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Detailed inspect | `kubectl describe pod <pod>` | Shows status, conditions, and events. |
| Schema help | `kubectl explain deployment.spec.template` | Reads API documentation from the cluster. |
| Output YAML | `kubectl get deploy <name> -o yaml` | Shows full live object. |
| Output JSONPath | `kubectl get pods -o jsonpath='{.items[*].metadata.name}'` | Extracts fields for scripts. |
| Generate YAML | `kubectl create deployment web --image=nginx --dry-run=client -o yaml > deployment.yaml` | Starter manifest pattern ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Apply file | `kubectl apply -f <file.yaml>` | Declarative create/update. |
| Apply directory | `kubectl apply -f ./manifests/` | Applies all manifests in a directory ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Preview apply | `kubectl diff -f ./manifests/` | Shows changes before apply ([Kubernetes declarative object management](https://kubernetes.io/docs/tasks/manage-kubernetes-objects/declarative-config/)). |
| Apply Kustomize | `kubectl apply -k ./overlays/prod` | Applies rendered Kustomize output ([Kubernetes Kustomize](https://kubernetes.io/docs/tasks/manage-kubernetes-objects/kustomization/)). |
| Logs | `kubectl logs <pod>` | Prints container logs. |
| Follow logs | `kubectl logs -f <pod>` | Streams logs ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Previous logs | `kubectl logs <pod> --previous` | Useful for crash loops ([Kubernetes troubleshooting applications](https://kubernetes.io/docs/tasks/debug/debug-application/)). |
| Logs by Deployment | `kubectl logs deploy/<deployment>` | Reads logs from a Deployment-backed Pod. |
| Exec shell | `kubectl exec -it <pod> -- sh` | Opens shell if the image contains one. |
| Port forward | `kubectl port-forward svc/<service> 8080:80` | Local testing through the API server. |
| Scale | `kubectl scale deployment/<name> --replicas=<n>` | Manual replica control. |
| Autoscale | `kubectl autoscale deployment <name> --min=2 --max=10 --cpu-percent=70` | Creates HPA ([Kubernetes horizontal pod autoscaling](https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/)). |
| HPA status | `kubectl get hpa` | Shows autoscaler targets and replicas. |
| Update image | `kubectl set image deployment/<name> <container>=<image>` | Triggers Deployment rollout if Pod template changes. |
| Rollout status | `kubectl rollout status deployment/<name>` | Waits for rollout progress. |
| Rollout history | `kubectl rollout history deployment/<name>` | Lists revisions. |
| Roll back | `kubectl rollout undo deployment/<name>` | Reverts to previous Deployment revision. |
| Create Job | `kubectl create job <name> --image=<image> -- <command>` | One-off batch run ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Job logs | `kubectl logs job/<name>` | Reads logs from Job Pods ([Kubernetes Jobs](https://kubernetes.io/docs/concepts/workloads/controllers/job/)). |
| Create CronJob | `kubectl create cronjob <name> --image=<image> --schedule='*/5 * * * *' -- <command>` | Scheduled batch run ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Create ConfigMap | `kubectl create configmap <name> --from-literal=KEY=value` | Non-secret configuration. |
| Create Secret | `kubectl create secret generic <name> --from-literal=password=...` | Sensitive configuration. |
| Events | `kubectl get events --sort-by=.metadata.creationTimestamp` | Timeline for troubleshooting ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Warning events | `kubectl events --types=Warning` | Focus on warnings ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Debug container | `kubectl debug -it <pod> --image=busybox:1.28 --target=<container>` | Adds ephemeral debug container ([Kubernetes debug running Pods](https://kubernetes.io/docs/tasks/debug/debug-application/debug-running-pod/)). |
| Check permission | `kubectl auth can-i create deployments --namespace <ns>` | Tests RBAC access ([Kubernetes authorization](https://kubernetes.io/docs/reference/access-authn-authz/authorization/)). |
| Cordon node | `kubectl cordon <node>` | Stops new scheduling on a node ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Drain node | `kubectl drain <node> --ignore-daemonsets --delete-emptydir-data` | Evicts Pods before maintenance. |
| Uncordon node | `kubectl uncordon <node>` | Allows scheduling again ([Kubernetes kubectl quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)). |
| Delete from file | `kubectl delete -f <file.yaml>` | Cleans up manifest-managed resources. |
| Delete by label | `kubectl delete deploy,svc -l app=web` | Cleans grouped objects. |
| Delete namespace | `kubectl delete namespace <ns>` | Removes namespaced resources; use carefully. |
