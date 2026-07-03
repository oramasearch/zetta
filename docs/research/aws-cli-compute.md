# Research: AWS CLI Compute Guide

> **Provenance.** Captured 2026-06-26 from a user-supplied hands-on guide
> ("AWS CLI Compute Guide: Launching Workloads on AWS with `aws`"), whose command
> syntax cites the official AWS CLI command reference and AWS service
> documentation (primary-source links are inline below).
>
> **Status:** reference / background. It is the cited basis for the `aws`
> command shapes the **AWS provisioner** uses — `AwsCliControlPlane`
> (`src/core/cloud/aws_cli_control_plane.cpp`); see
> [aws-provisioner.md](../cloud/aws-provisioner.md). zetta's burst backend uses only the
> **EC2 Auto Scaling Group + Spot** slice (§2.9–2.10); the rest (EC2 basics, ECS
> on Fargate, EKS, Batch, Lambda, App Runner, GPU/HPC) is background for choosing
> primitives and for the Azure/GCP equivalents in
> [cloud-provisioner-pattern.md](../cloud/cloud-provisioner-pattern.md). Examples are
> illustrative — replace account ids, subnet/security-group ids, regions, AMIs,
> role ARNs, and key names with values your account allows.

---

# AWS CLI Compute Guide: Launching Workloads on AWS with `aws`

This hands-on guide shows practical AWS CLI commands for launching compute workloads across EC2 virtual machines, ECS on Fargate, EKS Kubernetes clusters, AWS Batch, Lambda, App Runner, and GPU/HPC options, using official AWS CLI and service documentation as the syntax baseline. [AWS CLI command reference](https://docs.aws.amazon.com/cli/latest/reference/) It assumes a Bash-like shell, AWS CLI v2, and IAM permissions to create the resources shown. [AWS CLI installation guide](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html) [AWS IAM best practices](https://docs.aws.amazon.com/IAM/latest/UserGuide/best-practices.html)

> Replace placeholder values such as `subnet-0123456789abcdef0`, `sg-0123456789abcdef0`, account IDs, role ARNs, key names, and regions before running commands.

## 0. Choose the right AWS compute option

| Workload | Good default | Why use it | Main AWS CLI commands |
| --- | --- | --- | --- |
| General-purpose VM, full OS control, custom networking, SSH access | Amazon EC2 | EC2 launches virtual machine instances and exposes lifecycle actions such as run, start, stop, and terminate through the AWS CLI command reference for EC2 instances. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html) | `aws ec2 run-instances`, `aws ec2 stop-instances`, `aws ssm send-command` |
| Container task or service without managing servers | Amazon ECS on AWS Fargate | ECS Fargate runs container tasks with the `FARGATE` launch type and `awsvpc` networking, and task definitions declare CPU, memory, image, logs, and roles. [AWS CLI ECS `register-task-definition`](https://docs.aws.amazon.com/cli/latest/reference/ecs/register-task-definition.html) | `aws ecs create-cluster`, `aws ecs register-task-definition`, `aws ecs run-task`, `aws ecs create-service` |
| Kubernetes | Amazon EKS | EKS clusters can be created through `aws eks create-cluster`, then local Kubernetes config can be generated with `aws eks update-kubeconfig`. [AWS CLI EKS `create-cluster`](https://docs.aws.amazon.com/cli/latest/reference/eks/create-cluster.html) [AWS CLI EKS `update-kubeconfig`](https://docs.aws.amazon.com/cli/latest/reference/eks/update-kubeconfig.html) | `aws eks create-cluster`, `aws eks create-nodegroup`, `aws eks update-kubeconfig`, `kubectl apply` |
| Large-scale batch, arrays, queues, HPC-style job submission | AWS Batch | AWS Batch organizes workloads around compute environments, job queues, job definitions, and submitted jobs. [AWS Batch User Guide](https://docs.aws.amazon.com/batch/latest/userguide/what-is-batch.html) | `aws batch create-compute-environment`, `aws batch create-job-queue`, `aws batch register-job-definition`, `aws batch submit-job` |
| Event-driven function | AWS Lambda | Lambda functions can be created with code packages and invoked from the CLI with `aws lambda create-function` and `aws lambda invoke`. [AWS CLI Lambda `create-function`](https://docs.aws.amazon.com/cli/latest/reference/lambda/create-function.html) [AWS CLI Lambda `invoke`](https://docs.aws.amazon.com/cli/latest/reference/lambda/invoke.html) | `aws lambda create-function`, `aws lambda invoke` |
| Serverless web container | AWS App Runner | App Runner creates services from source configurations such as image repositories using `aws apprunner create-service`. [AWS CLI App Runner `create-service`](https://docs.aws.amazon.com/cli/latest/reference/apprunner/create-service.html) | `aws apprunner create-service` |

## 1. Getting started with AWS CLI

### 1.1 Install or update AWS CLI v2

AWS publishes installation instructions for AWS CLI v2 in the AWS CLI User Guide. [AWS CLI installation guide](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html)

```bash
# macOS with Homebrew
brew install awscli

# Linux x86_64: download, unzip, and install AWS CLI v2
curl "https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip" -o "awscliv2.zip"
unzip awscliv2.zip
sudo ./aws/install

# Verify the installed CLI
aws --version
```

### 1.2 Configure long-lived access keys for a named profile

The `aws configure` workflow writes settings such as access keys, default region, and output format to AWS CLI configuration files. [AWS CLI configuration guide](https://docs.aws.amazon.com/cli/latest/userguide/cli-chap-configure.html)

```bash
aws configure --profile dev

# Example prompts:
# AWS Access Key ID [None]: AKIA...
# AWS Secret Access Key [None]: ...
# Default region name [None]: us-west-2
# Default output format [None]: json
```

Named profiles let the same workstation hold multiple sets of AWS CLI settings, and the CLI stores configuration and credential values in shared config and credentials files. [AWS CLI config and credentials files](https://docs.aws.amazon.com/cli/latest/userguide/cli-configure-files.html)

```bash
# Use a profile for one command
aws sts get-caller-identity --profile dev

# Use a profile for the current shell session
export AWS_PROFILE=dev
aws sts get-caller-identity
```

### 1.3 Configure IAM Identity Center / SSO login

AWS CLI v2 supports IAM Identity Center configuration with `aws configure sso`, and the workflow creates an SSO-enabled profile. [AWS CLI SSO configuration guide](https://docs.aws.amazon.com/cli/latest/userguide/cli-configure-sso.html)

```bash
aws configure sso --profile corp-dev

# Then authenticate the profile in a browser-backed login flow
aws sso login --profile corp-dev

# Use it like any other profile
AWS_PROFILE=corp-dev aws sts get-caller-identity
```

### 1.4 Set default region and output format

The AWS CLI supports output formats including `json`, `yaml`, `yaml-stream`, `text`, and `table`. [AWS CLI output format guide](https://docs.aws.amazon.com/cli/latest/userguide/cli-usage-output-format.html)

```bash
# Set defaults on a profile
aws configure set region us-west-2 --profile dev
aws configure set output json --profile dev

# Override per command
aws ec2 describe-availability-zones --region us-east-1 --output table
```

### 1.5 Verify identity before creating resources

`aws sts get-caller-identity` returns details about the IAM identity used for the request, and AWS states that no permissions are required to perform this operation. [AWS CLI STS `get-caller-identity`](https://docs.aws.amazon.com/cli/latest/reference/sts/get-caller-identity.html)

```bash
aws sts get-caller-identity --profile dev --output json
```

Typical output:

```json
{
  "UserId": "AIDAEXAMPLE...",
  "Account": "123456789012",
  "Arn": "arn:aws:iam::123456789012:user/alex"
}
```

### 1.6 Recommended shell variables for the rest of the guide

The examples below use shell variables so commands stay copy-pasteable across accounts, VPCs, and regions.

```bash
export AWS_PROFILE=dev
export AWS_REGION=us-west-2
export VPC_ID=vpc-0123456789abcdef0
export SUBNET_ID=subnet-0123456789abcdef0
export SUBNET_ID_2=subnet-0abcdef1234567890
export SECURITY_GROUP_ID=sg-0123456789abcdef0
export KEY_NAME=my-ec2-keypair
export ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
```

## 2. Amazon EC2: virtual machines

### 2.1 Find a current Amazon Linux AMI

AWS publishes public Systems Manager Parameter Store parameters for current Amazon Linux AMIs, which avoids hard-coding AMI IDs that vary by region. [Systems Manager public AMI parameters](https://docs.aws.amazon.com/systems-manager/latest/userguide/parameter-store-public-parameters-ami.html)

```bash
export AMI_ID=$(aws ssm get-parameter \
  --name /aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-x86_64 \
  --query 'Parameter.Value' \
  --output text)

echo "$AMI_ID"
```

`aws ec2 describe-images` can also filter Amazon-owned AMIs and sort results by `CreationDate`. [AWS CLI EC2 `describe-images`](https://docs.aws.amazon.com/cli/latest/reference/ec2/describe-images.html)

```bash
aws ec2 describe-images \
  --owners amazon \
  --filters \
    "Name=name,Values=al2023-ami-2023.*-x86_64" \
    "Name=architecture,Values=x86_64" \
    "Name=virtualization-type,Values=hvm" \
  --query 'Images | sort_by(@, &CreationDate)[-1].{ImageId:ImageId,Name:Name,CreationDate:CreationDate}' \
  --output table
```

### 2.2 Find EC2 instance types

`aws ec2 describe-instance-types` returns instance type metadata, including CPU, memory, supported architectures, and other capabilities. [AWS CLI EC2 `describe-instance-types`](https://docs.aws.amazon.com/cli/latest/reference/ec2/describe-instance-types.html)

```bash
# Show common x86 burstable instance types and memory
aws ec2 describe-instance-types \
  --filters "Name=processor-info.supported-architecture,Values=x86_64" \
  --query 'InstanceTypes[?starts_with(InstanceType, `t3.`)].{Type:InstanceType,vCPU:VCpuInfo.DefaultVCpus,MemoryMiB:MemoryInfo.SizeInMiB}' \
  --output table

# Check GPU instance families in the current region's catalog
aws ec2 describe-instance-types \
  --filters "Name=instance-type,Values=g5.*,g6.*,p4d.*,p5.*" \
  --query 'InstanceTypes[].{Type:InstanceType,vCPU:VCpuInfo.DefaultVCpus,MemoryMiB:MemoryInfo.SizeInMiB,GPU:GpuInfo.Gpus[0].Name,GPUCount:GpuInfo.Gpus[0].Count}' \
  --output table
```

### 2.3 Create a security group for SSH or web traffic

Security groups are referenced by EC2 launch commands through `--security-group-ids`, and the instance launch command supports subnet and security group placement options. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)

```bash
export SECURITY_GROUP_ID=$(aws ec2 create-security-group \
  --group-name cli-compute-demo-sg \
  --description "AWS CLI compute demo" \
  --vpc-id "$VPC_ID" \
  --query 'GroupId' \
  --output text)

# Allow SSH from your current public IP only
export MY_IP=$(curl -s https://checkip.amazonaws.com)/32
aws ec2 authorize-security-group-ingress \
  --group-id "$SECURITY_GROUP_ID" \
  --protocol tcp \
  --port 22 \
  --cidr "$MY_IP"

# Optional: allow HTTP for web demos
aws ec2 authorize-security-group-ingress \
  --group-id "$SECURITY_GROUP_ID" \
  --protocol tcp \
  --port 80 \
  --cidr 0.0.0.0/0
```

### 2.4 Launch an EC2 instance with common options

`aws ec2 run-instances` launches instances and supports options for AMI, instance type, key pair, security groups, subnet, user data, IAM instance profile, and tags. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)

Create optional user data:

```bash
cat > user-data.sh <<'EOF'
#!/bin/bash
set -euxo pipefail
dnf update -y
dnf install -y nginx
systemctl enable --now nginx
echo "hello from $(hostname)" > /usr/share/nginx/html/index.html
EOF
```

Launch the instance:

```bash
export INSTANCE_ID=$(aws ec2 run-instances \
  --image-id "$AMI_ID" \
  --instance-type t3.micro \
  --count 1 \
  --key-name "$KEY_NAME" \
  --security-group-ids "$SECURITY_GROUP_ID" \
  --subnet-id "$SUBNET_ID" \
  --user-data file://user-data.sh \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=cli-ec2-demo},{Key=Project,Value=aws-cli-compute-guide}]' \
  --query 'Instances[0].InstanceId' \
  --output text)

echo "$INSTANCE_ID"
aws ec2 wait instance-running --instance-ids "$INSTANCE_ID"
```

Key flags:

- `--image-id` selects the AMI used as the instance boot image. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)
- `--instance-type` selects the EC2 hardware shape, such as `t3.micro`, `m7i.large`, `g5.xlarge`, or `p5.48xlarge`. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)
- `--key-name` associates an EC2 key pair for SSH-based login. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)
- `--security-group-ids` and `--subnet-id` place the instance into a VPC security boundary and subnet. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)
- `--user-data file://user-data.sh` passes bootstrapping data to the instance at launch. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)
- `--tag-specifications` applies tags during creation, and AWS recommends tagging to categorize and manage AWS resources. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html) [AWS tagging guide](https://docs.aws.amazon.com/tag-editor/latest/userguide/tagging.html)

### 2.5 Inspect instance status and connection details

`aws ec2 describe-instances` can query instance state and public addressing details after launch. [AWS CLI EC2 `run-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/run-instances.html)

```bash
aws ec2 describe-instances \
  --instance-ids "$INSTANCE_ID" \
  --query 'Reservations[0].Instances[0].{State:State.Name,PublicIp:PublicIpAddress,PrivateIp:PrivateIpAddress,AZ:Placement.AvailabilityZone}' \
  --output table

export PUBLIC_IP=$(aws ec2 describe-instances \
  --instance-ids "$INSTANCE_ID" \
  --query 'Reservations[0].Instances[0].PublicIpAddress' \
  --output text)
```

### 2.6 Connect by SSH, EC2 Instance Connect, or SSM Session Manager

Traditional SSH requires network path to TCP port 22, the correct OS user, and the private key corresponding to the EC2 key pair used at launch.

```bash
ssh -i ~/.ssh/my-ec2-keypair.pem ec2-user@"$PUBLIC_IP"
```

The AWS CLI includes `aws ec2-instance-connect ssh`, whose synopsis requires `--instance-id` and supports options such as `--private-key-file`, `--os-user`, and `--connection-type`. [AWS CLI EC2 Instance Connect `ssh`](https://docs.aws.amazon.com/cli/latest/reference/ec2-instance-connect/ssh.html)

```bash
aws ec2-instance-connect ssh \
  --instance-id "$INSTANCE_ID" \
  --os-user ec2-user \
  --private-key-file ~/.ssh/my-ec2-keypair.pem
```

SSM Session Manager starts an interactive session with `aws ssm start-session`, and the command requires a target managed node ID. [AWS CLI SSM `start-session`](https://docs.aws.amazon.com/cli/latest/reference/ssm/start-session.html)

```bash
# Requires the instance to be a managed node with the SSM Agent, IAM permissions, and network access to Systems Manager endpoints.
aws ssm start-session --target "$INSTANCE_ID"
```

### 2.7 Run commands on an EC2 instance with SSM Run Command

`aws ssm send-command` can run a document such as `AWS-RunShellScript` on managed nodes, and it accepts target instance IDs and command parameters. [AWS CLI SSM `send-command`](https://docs.aws.amazon.com/cli/latest/reference/ssm/send-command.html)

```bash
export COMMAND_ID=$(aws ssm send-command \
  --instance-ids "$INSTANCE_ID" \
  --document-name "AWS-RunShellScript" \
  --comment "check nginx" \
  --parameters 'commands=["systemctl is-active nginx","curl -s http://localhost"]' \
  --query 'Command.CommandId' \
  --output text)

aws ssm get-command-invocation \
  --command-id "$COMMAND_ID" \
  --instance-id "$INSTANCE_ID" \
  --query '{Status:Status,Output:StandardOutputContent,Error:StandardErrorContent}' \
  --output json
```

### 2.8 Stop, start, and terminate EC2 instances

The AWS CLI exposes separate lifecycle commands for starting, stopping, and terminating EC2 instances. [AWS CLI EC2 `start-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/start-instances.html) [AWS CLI EC2 `stop-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/stop-instances.html) [AWS CLI EC2 `terminate-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/terminate-instances.html)

```bash
# Stop preserves the instance record and attached EBS volumes unless delete-on-termination rules apply.
aws ec2 stop-instances --instance-ids "$INSTANCE_ID"
aws ec2 wait instance-stopped --instance-ids "$INSTANCE_ID"

# Start a stopped instance.
aws ec2 start-instances --instance-ids "$INSTANCE_ID"
aws ec2 wait instance-running --instance-ids "$INSTANCE_ID"

# Terminate permanently deletes the instance resource.
aws ec2 terminate-instances --instance-ids "$INSTANCE_ID"
aws ec2 wait instance-terminated --instance-ids "$INSTANCE_ID"
```

### 2.9 Scale EC2 with Auto Scaling Groups

An Auto Scaling group can launch and maintain a fleet using a launch template, and AWS provides a workflow for creating Auto Scaling groups from launch templates. [Amazon EC2 Auto Scaling launch template guide](https://docs.aws.amazon.com/autoscaling/ec2/userguide/create-asg-launch-template.html)

```bash
# Create a launch template for repeated EC2 launches.
export USER_DATA_B64=$(python3 -c 'import base64; print(base64.b64encode(open("user-data.sh","rb").read()).decode())')

cat > launch-template-data.json <<EOF
{
  "ImageId": "$AMI_ID",
  "InstanceType": "t3.micro",
  "KeyName": "$KEY_NAME",
  "SecurityGroupIds": ["$SECURITY_GROUP_ID"],
  "UserData": "$USER_DATA_B64"
}
EOF

aws ec2 create-launch-template \
  --launch-template-name cli-asg-template \
  --version-description v1 \
  --launch-template-data file://launch-template-data.json

# Create an Auto Scaling group across two subnets.
aws autoscaling create-auto-scaling-group \
  --auto-scaling-group-name cli-web-asg \
  --launch-template LaunchTemplateName=cli-asg-template,Version=1 \
  --min-size 1 \
  --max-size 4 \
  --desired-capacity 2 \
  --vpc-zone-identifier "$SUBNET_ID,$SUBNET_ID_2" \
  --tags "Key=Project,Value=aws-cli-compute-guide,PropagateAtLaunch=true"

# Scale it later.
aws autoscaling set-desired-capacity \
  --auto-scaling-group-name cli-web-asg \
  --desired-capacity 3
```

### 2.10 Use Spot Instances for cost-sensitive EC2 workloads

Spot Instances use spare EC2 capacity and are suitable for flexible, interruptible workloads. [Amazon EC2 Spot Instances guide](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/using-spot-instances.html)

`aws ec2 request-spot-instances` requests Spot capacity and accepts launch specifications such as image ID, instance type, key name, security groups, and subnet. [AWS CLI EC2 `request-spot-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/request-spot-instances.html)

```bash
cat > spot-launch-spec.json <<EOF
{
  "ImageId": "$AMI_ID",
  "InstanceType": "t3.micro",
  "KeyName": "$KEY_NAME",
  "SecurityGroupIds": ["$SECURITY_GROUP_ID"],
  "SubnetId": "$SUBNET_ID"
}
EOF

aws ec2 request-spot-instances \
  --instance-count 1 \
  --type one-time \
  --launch-specification file://spot-launch-spec.json
```

For production fleets, prefer Auto Scaling groups or EC2 Fleet with allocation strategies rather than hand-managing many one-off Spot requests.

## 3. AWS Fargate with Amazon ECS: serverless containers

### 3.1 Create an ECS cluster and CloudWatch log group

`aws ecs create-cluster` creates an ECS cluster, and `aws logs tail` can stream CloudWatch Logs log groups from the CLI. [AWS CLI ECS `create-cluster`](https://docs.aws.amazon.com/cli/latest/reference/ecs/create-cluster.html) [AWS CLI CloudWatch Logs `tail`](https://docs.aws.amazon.com/cli/latest/reference/logs/tail.html)

```bash
aws ecs create-cluster --cluster-name cli-fargate-demo

aws logs create-log-group --log-group-name /ecs/cli-fargate-demo || true
```

### 3.2 Register a Fargate task definition

For Fargate tasks, the ECS task definition uses `requiresCompatibilities` set to `FARGATE`, `networkMode` set to `awsvpc`, and task-level `cpu` and `memory` values. [AWS CLI ECS `register-task-definition`](https://docs.aws.amazon.com/cli/latest/reference/ecs/register-task-definition.html)

```bash
export ECS_TASK_EXECUTION_ROLE_ARN=arn:aws:iam::$ACCOUNT_ID:role/ecsTaskExecutionRole

cat > ecs-task-def.json <<EOF
{
  "family": "cli-nginx",
  "networkMode": "awsvpc",
  "requiresCompatibilities": ["FARGATE"],
  "cpu": "256",
  "memory": "512",
  "executionRoleArn": "$ECS_TASK_EXECUTION_ROLE_ARN",
  "containerDefinitions": [
    {
      "name": "nginx",
      "image": "public.ecr.aws/docker/library/nginx:latest",
      "essential": true,
      "portMappings": [
        {"containerPort": 80, "protocol": "tcp"}
      ],
      "logConfiguration": {
        "logDriver": "awslogs",
        "options": {
          "awslogs-group": "/ecs/cli-fargate-demo",
          "awslogs-region": "$AWS_REGION",
          "awslogs-stream-prefix": "ecs"
        }
      }
    }
  ]
}
EOF

aws ecs register-task-definition --cli-input-json file://ecs-task-def.json
```

### 3.3 Run a one-off Fargate task

`aws ecs run-task` can start tasks on a cluster, and the Fargate launch type uses `awsvpcConfiguration` with subnets, security groups, and optional public IP assignment. [AWS CLI ECS `run-task`](https://docs.aws.amazon.com/cli/latest/reference/ecs/run-task.html)

```bash
aws ecs run-task \
  --cluster cli-fargate-demo \
  --launch-type FARGATE \
  --task-definition cli-nginx \
  --network-configuration "awsvpcConfiguration={subnets=[$SUBNET_ID],securityGroups=[$SECURITY_GROUP_ID],assignPublicIp=ENABLED}" \
  --count 1
```

### 3.4 Create a long-running ECS service on Fargate

`aws ecs create-service` creates an ECS service from a task definition and can maintain the requested desired task count. [AWS CLI ECS `create-service`](https://docs.aws.amazon.com/cli/latest/reference/ecs/create-service.html)

```bash
aws ecs create-service \
  --cluster cli-fargate-demo \
  --service-name cli-nginx-service \
  --task-definition cli-nginx \
  --desired-count 2 \
  --launch-type FARGATE \
  --network-configuration "awsvpcConfiguration={subnets=[$SUBNET_ID,$SUBNET_ID_2],securityGroups=[$SECURITY_GROUP_ID],assignPublicIp=ENABLED}"

aws ecs describe-services \
  --cluster cli-fargate-demo \
  --services cli-nginx-service \
  --query 'services[0].{Status:status,Desired:desiredCount,Running:runningCount,Pending:pendingCount}' \
  --output table
```

### 3.5 View container logs

`aws logs tail` supports following a CloudWatch Logs log group and filtering by a time window. [AWS CLI CloudWatch Logs `tail`](https://docs.aws.amazon.com/cli/latest/reference/logs/tail.html)

```bash
aws logs tail /ecs/cli-fargate-demo --since 10m --follow
```

### 3.6 Clean up ECS resources

ECS services should be scaled down or deleted before deleting the cluster.

```bash
aws ecs update-service \
  --cluster cli-fargate-demo \
  --service cli-nginx-service \
  --desired-count 0

aws ecs delete-service \
  --cluster cli-fargate-demo \
  --service cli-nginx-service \
  --force

aws ecs delete-cluster --cluster cli-fargate-demo
```

## 4. Amazon EKS: Kubernetes

### 4.1 Use `eksctl` for day-one cluster creation when possible

AWS documents `eksctl` as one of the ways to create an Amazon EKS cluster. [Amazon EKS cluster creation guide](https://docs.aws.amazon.com/eks/latest/userguide/create-cluster.html)

```bash
# Install eksctl separately, then create a managed-node EKS cluster.
eksctl create cluster \
  --name cli-eks-demo \
  --region "$AWS_REGION" \
  --nodes 2 \
  --node-type t3.medium \
  --managed
```

### 4.2 Create an EKS control plane with `aws eks create-cluster`

`aws eks create-cluster` creates the EKS control plane and requires a cluster name, IAM role ARN, and VPC resource configuration. [AWS CLI EKS `create-cluster`](https://docs.aws.amazon.com/cli/latest/reference/eks/create-cluster.html)

```bash
export EKS_CLUSTER_ROLE_ARN=arn:aws:iam::$ACCOUNT_ID:role/eksClusterRole

aws eks create-cluster \
  --name cli-eks-demo \
  --role-arn "$EKS_CLUSTER_ROLE_ARN" \
  --resources-vpc-config subnetIds="$SUBNET_ID,$SUBNET_ID_2",securityGroupIds="$SECURITY_GROUP_ID",endpointPublicAccess=true \
  --tags Project=aws-cli-compute-guide

aws eks wait cluster-active --name cli-eks-demo
```

### 4.3 Configure `kubectl`

`aws eks update-kubeconfig` creates or updates kubeconfig entries for an EKS cluster. [AWS CLI EKS `update-kubeconfig`](https://docs.aws.amazon.com/cli/latest/reference/eks/update-kubeconfig.html)

```bash
aws eks update-kubeconfig \
  --name cli-eks-demo \
  --region "$AWS_REGION" \
  --alias cli-eks-demo

kubectl get nodes
```

### 4.4 Add a managed node group

`aws eks create-nodegroup` creates a managed node group and accepts cluster name, node group name, subnet IDs, node IAM role, scaling configuration, AMI type, and instance types. [AWS CLI EKS `create-nodegroup`](https://docs.aws.amazon.com/cli/latest/reference/eks/create-nodegroup.html)

```bash
export EKS_NODE_ROLE_ARN=arn:aws:iam::$ACCOUNT_ID:role/eksNodeRole

aws eks create-nodegroup \
  --cluster-name cli-eks-demo \
  --nodegroup-name cli-eks-ng \
  --subnets "$SUBNET_ID" "$SUBNET_ID_2" \
  --node-role "$EKS_NODE_ROLE_ARN" \
  --scaling-config minSize=1,maxSize=3,desiredSize=2 \
  --instance-types t3.medium \
  --ami-type AL2023_x86_64_STANDARD \
  --tags Project=aws-cli-compute-guide

aws eks wait nodegroup-active \
  --cluster-name cli-eks-demo \
  --nodegroup-name cli-eks-ng
```

### 4.5 Deploy a workload

After kubeconfig is updated, Kubernetes workloads are deployed with `kubectl` while AWS CLI manages the EKS cluster and node groups. [AWS CLI EKS `update-kubeconfig`](https://docs.aws.amazon.com/cli/latest/reference/eks/update-kubeconfig.html)

```bash
kubectl create deployment hello-nginx --image=nginx:latest
kubectl expose deployment hello-nginx --type=LoadBalancer --port=80 --target-port=80
kubectl get service hello-nginx --watch
```

### 4.6 Clean up EKS resources

Delete Kubernetes services that create load balancers before deleting node groups and clusters to avoid orphaned resources.

```bash
kubectl delete service hello-nginx || true
kubectl delete deployment hello-nginx || true

aws eks delete-nodegroup \
  --cluster-name cli-eks-demo \
  --nodegroup-name cli-eks-ng

aws eks wait nodegroup-deleted \
  --cluster-name cli-eks-demo \
  --nodegroup-name cli-eks-ng

aws eks delete-cluster --name cli-eks-demo
```

## 5. AWS Batch: parallel and HPC-style job queues

AWS Batch is designed for batch computing and uses compute environments, job queues, job definitions, and submitted jobs. [AWS Batch User Guide](https://docs.aws.amazon.com/batch/latest/userguide/what-is-batch.html)

### 5.1 Create a Fargate compute environment

`aws batch create-compute-environment` supports managed compute environments and Fargate compute resource type values such as `FARGATE` and `FARGATE_SPOT`. [AWS CLI Batch `create-compute-environment`](https://docs.aws.amazon.com/cli/latest/reference/batch/create-compute-environment.html)

```bash
cat > batch-compute-env.json <<EOF
{
  "computeEnvironmentName": "cli-batch-fargate",
  "type": "MANAGED",
  "state": "ENABLED",
  "computeResources": {
    "type": "FARGATE",
    "maxvCpus": 16,
    "subnets": ["$SUBNET_ID", "$SUBNET_ID_2"],
    "securityGroupIds": ["$SECURITY_GROUP_ID"]
  },
  "tags": {
    "Project": "aws-cli-compute-guide"
  }
}
EOF

aws batch create-compute-environment --cli-input-json file://batch-compute-env.json
```

### 5.2 Create a job queue

`aws batch create-job-queue` creates the queue that maps submitted jobs to one or more compute environments. [AWS CLI Batch `create-job-queue`](https://docs.aws.amazon.com/cli/latest/reference/batch/create-job-queue.html)

```bash
cat > batch-job-queue.json <<EOF
{
  "jobQueueName": "cli-batch-queue",
  "state": "ENABLED",
  "priority": 1,
  "computeEnvironmentOrder": [
    {"order": 1, "computeEnvironment": "cli-batch-fargate"}
  ],
  "tags": {
    "Project": "aws-cli-compute-guide"
  }
}
EOF

aws batch create-job-queue --cli-input-json file://batch-job-queue.json
```

### 5.3 Register a job definition

`aws batch register-job-definition` registers the reusable definition of a job, and Fargate jobs use `platformCapabilities` with `FARGATE` plus container properties such as image, command, execution role, resource requirements, network configuration, and log configuration. [AWS CLI Batch `register-job-definition`](https://docs.aws.amazon.com/cli/latest/reference/batch/register-job-definition.html)

```bash
export BATCH_EXECUTION_ROLE_ARN=arn:aws:iam::$ACCOUNT_ID:role/ecsTaskExecutionRole

cat > batch-job-def.json <<EOF
{
  "jobDefinitionName": "cli-batch-hello",
  "type": "container",
  "platformCapabilities": ["FARGATE"],
  "containerProperties": {
    "image": "public.ecr.aws/docker/library/busybox:latest",
    "command": ["sh", "-c", "echo hello from AWS Batch; sleep 30"],
    "executionRoleArn": "$BATCH_EXECUTION_ROLE_ARN",
    "resourceRequirements": [
      {"type": "VCPU", "value": "0.25"},
      {"type": "MEMORY", "value": "512"}
    ],
    "networkConfiguration": {
      "assignPublicIp": "ENABLED"
    },
    "logConfiguration": {
      "logDriver": "awslogs"
    }
  },
  "tags": {
    "Project": "aws-cli-compute-guide"
  }
}
EOF

aws batch register-job-definition --cli-input-json file://batch-job-def.json
```

### 5.4 Submit and inspect a job

`aws batch submit-job` submits work by job name, job queue, and job definition. [AWS CLI Batch `submit-job`](https://docs.aws.amazon.com/cli/latest/reference/batch/submit-job.html)

```bash
export JOB_ID=$(aws batch submit-job \
  --job-name hello-batch \
  --job-queue cli-batch-queue \
  --job-definition cli-batch-hello \
  --query 'jobId' \
  --output text)

echo "$JOB_ID"

aws batch describe-jobs \
  --jobs "$JOB_ID" \
  --query 'jobs[0].{Status:status,Queue:jobQueue,Definition:jobDefinition,StartedAt:startedAt,StoppedAt:stoppedAt}' \
  --output json
```

For many similar jobs, use Batch array jobs with `--array-properties size=N` on `submit-job`. [AWS CLI Batch `submit-job`](https://docs.aws.amazon.com/cli/latest/reference/batch/submit-job.html)

```bash
aws batch submit-job \
  --job-name array-demo \
  --job-queue cli-batch-queue \
  --job-definition cli-batch-hello \
  --array-properties size=10
```

### 5.5 Clean up AWS Batch resources

Disable queues and compute environments before deleting them.

```bash
aws batch update-job-queue --job-queue cli-batch-queue --state DISABLED
aws batch delete-job-queue --job-queue cli-batch-queue

aws batch update-compute-environment \
  --compute-environment cli-batch-fargate \
  --state DISABLED
aws batch delete-compute-environment \
  --compute-environment cli-batch-fargate
```

## 6. Serverless compute options

### 6.1 AWS Lambda: create and invoke a function

`aws lambda create-function` creates a function from a deployment package and requires details such as function name, runtime, IAM role, handler, and code package. [AWS CLI Lambda `create-function`](https://docs.aws.amazon.com/cli/latest/reference/lambda/create-function.html)

```bash
cat > lambda_function.py <<'EOF'
def lambda_handler(event, context):
    name = event.get("name", "world")
    return {"message": f"hello {name}"}
EOF

zip function.zip lambda_function.py

export LAMBDA_ROLE_ARN=arn:aws:iam::$ACCOUNT_ID:role/lambda-basic-execution-role

aws lambda create-function \
  --function-name cli-hello \
  --runtime python3.12 \
  --role "$LAMBDA_ROLE_ARN" \
  --handler lambda_function.lambda_handler \
  --zip-file fileb://function.zip \
  --tags Project=aws-cli-compute-guide
```

`aws lambda invoke` invokes a Lambda function and writes the response payload to a local output file. [AWS CLI Lambda `invoke`](https://docs.aws.amazon.com/cli/latest/reference/lambda/invoke.html)

```bash
aws lambda invoke \
  --function-name cli-hello \
  --payload '{"name":"AWS CLI"}' \
  --cli-binary-format raw-in-base64-out \
  response.json

cat response.json
```

### 6.2 AWS App Runner: serverless container web service

`aws apprunner create-service` creates an App Runner service and requires a service name plus a source configuration. [AWS CLI App Runner `create-service`](https://docs.aws.amazon.com/cli/latest/reference/apprunner/create-service.html) Amazon ECR Public images are publicly readable for App Runner services and do not require an access role, and App Runner does not support automatic deployments for Amazon ECR Public images. [AWS App Runner image source guide](https://docs.aws.amazon.com/apprunner/latest/dg/service-source-image.html)

```bash
cat > apprunner-service.json <<'EOF'
{
  "ServiceName": "cli-apprunner-hello",
  "SourceConfiguration": {
    "AutoDeploymentsEnabled": false,
    "ImageRepository": {
      "ImageIdentifier": "public.ecr.aws/aws-containers/hello-app-runner:latest",
      "ImageRepositoryType": "ECR_PUBLIC",
      "ImageConfiguration": {
        "Port": "8000"
      }
    }
  },
  "InstanceConfiguration": {
    "Cpu": "1 vCPU",
    "Memory": "2 GB"
  },
  "Tags": [
    {"Key": "Project", "Value": "aws-cli-compute-guide"}
  ]
}
EOF

aws apprunner create-service --cli-input-json file://apprunner-service.json
```

Use App Runner when the workload is an HTTP service packaged as a container and the goal is to avoid managing ECS services, load balancers, or Kubernetes objects. [AWS App Runner image source guide](https://docs.aws.amazon.com/apprunner/latest/dg/service-source-image.html)

## 7. GPU and HPC workloads

### 7.1 Choose GPU instance families intentionally

Amazon EC2 accelerated computing instances include GPU-oriented families such as P-series and G-series, and AWS documents accelerated computing instance specifications separately from general-purpose families. [Amazon EC2 accelerated computing instances](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/accelerated-computing-instances.html)

```bash
aws ec2 describe-instance-types \
  --filters "Name=instance-type,Values=g5.*,g6.*,p4d.*,p5.*" \
  --query 'InstanceTypes[].{Type:InstanceType,vCPU:VCpuInfo.DefaultVCpus,MemoryMiB:MemoryInfo.SizeInMiB,GPU:GpuInfo.Gpus[0].Name,GPUCount:GpuInfo.Gpus[0].Count,Network:NetworkInfo.NetworkPerformance}' \
  --output table
```

### 7.2 Use Deep Learning AMIs for ML/AI bootstrapping

AWS Deep Learning AMIs provide machine learning frameworks, dependencies, and GPU software for EC2-based deep learning workloads. [AWS Deep Learning AMI guide](https://docs.aws.amazon.com/dlami/latest/devguide/what-is-dlami.html)

```bash
aws ec2 describe-images \
  --owners amazon \
  --filters \
    "Name=name,Values=Deep Learning AMI GPU*Ubuntu*" \
    "Name=architecture,Values=x86_64" \
    "Name=state,Values=available" \
  --query 'Images | sort_by(@, &CreationDate)[-5:].{ImageId:ImageId,Name:Name,CreationDate:CreationDate}' \
  --output table
```

### 7.3 Launch a GPU instance

GPU capacity can be region- and Availability-Zone-sensitive, so production ML workloads often combine capacity reservations, diversified instance types, queues, or managed services rather than relying on a single large instance type.

```bash
export GPU_AMI_ID=ami-0123456789abcdef0

aws ec2 run-instances \
  --image-id "$GPU_AMI_ID" \
  --instance-type g5.xlarge \
  --count 1 \
  --key-name "$KEY_NAME" \
  --security-group-ids "$SECURITY_GROUP_ID" \
  --subnet-id "$SUBNET_ID" \
  --block-device-mappings '[{"DeviceName":"/dev/sda1","Ebs":{"VolumeSize":200,"VolumeType":"gp3","DeleteOnTermination":true}}]' \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=cli-gpu-demo},{Key=Project,Value=aws-cli-compute-guide}]'
```

For repeated GPU work, consider ECS GPU tasks on EC2 capacity, EKS GPU node groups, AWS Batch GPU jobs, or managed ML services such as SageMaker depending on scheduling, observability, and quota requirements.

## 8. Best practices

### 8.1 Cost management

Stop EC2 instances when you need to preserve the instance and attached volumes for later use, and terminate them when the workload and instance record are no longer needed. [AWS CLI EC2 `stop-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/stop-instances.html) [AWS CLI EC2 `terminate-instances`](https://docs.aws.amazon.com/cli/latest/reference/ec2/terminate-instances.html)

```bash
# Find running instances tagged for this guide
aws ec2 describe-instances \
  --filters "Name=tag:Project,Values=aws-cli-compute-guide" "Name=instance-state-name,Values=running" \
  --query 'Reservations[].Instances[].{Id:InstanceId,Type:InstanceType,Name:Tags[?Key==`Name`]|[0].Value,Launch:LaunchTime}' \
  --output table

# Stop all matching running instances after hours
aws ec2 stop-instances \
  --instance-ids $(aws ec2 describe-instances \
    --filters "Name=tag:Project,Values=aws-cli-compute-guide" "Name=instance-state-name,Values=running" \
    --query 'Reservations[].Instances[].InstanceId' \
    --output text)
```

Use Spot for fault-tolerant workloads, but design for interruption because Spot capacity is spare capacity. [Amazon EC2 Spot Instances guide](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/using-spot-instances.html)

### 8.2 Tag everything

AWS tagging helps categorize resources by purpose, owner, environment, cost center, or automation lifecycle. [AWS tagging guide](https://docs.aws.amazon.com/tag-editor/latest/userguide/tagging.html)

```bash
aws ec2 create-tags \
  --resources "$INSTANCE_ID" \
  --tags Key=Owner,Value=platform-team Key=Environment,Value=dev Key=AutoStop,Value=true
```

### 8.3 Prefer IAM least privilege and short-lived credentials

AWS recommends applying least-privilege permissions and following IAM security best practices. [AWS IAM best practices](https://docs.aws.amazon.com/IAM/latest/UserGuide/best-practices.html)

Practical implications:

- Use IAM Identity Center / SSO profiles for human users when available. [AWS CLI SSO configuration guide](https://docs.aws.amazon.com/cli/latest/userguide/cli-configure-sso.html)
- Use instance roles, task roles, execution roles, Lambda roles, and Batch execution roles instead of embedding access keys in user data or container images. [AWS IAM best practices](https://docs.aws.amazon.com/IAM/latest/UserGuide/best-practices.html)
- Scope permissions to the service and resource patterns required by the workload rather than broad administrator access. [AWS IAM best practices](https://docs.aws.amazon.com/IAM/latest/UserGuide/best-practices.html)

### 8.4 Use waiters and queries for safer automation

Many AWS CLI examples combine `aws ... wait` commands with `--query` to avoid racing ahead before a resource is ready, and the AWS CLI supports JMESPath queries through the global `--query` option across commands. [AWS CLI STS `get-caller-identity`](https://docs.aws.amazon.com/cli/latest/reference/sts/get-caller-identity.html)

```bash
aws ec2 wait instance-running --instance-ids "$INSTANCE_ID"
aws ec2 describe-instances \
  --instance-ids "$INSTANCE_ID" \
  --query 'Reservations[0].Instances[0].State.Name' \
  --output text
```

### 8.5 Common troubleshooting checks

- Confirm the active identity and account with `aws sts get-caller-identity` before creating or deleting resources. [AWS CLI STS `get-caller-identity`](https://docs.aws.amazon.com/cli/latest/reference/sts/get-caller-identity.html)
- Confirm region mismatches by checking `aws configure get region` and explicit `--region` flags. [AWS CLI configuration guide](https://docs.aws.amazon.com/cli/latest/userguide/cli-chap-configure.html)
- For EC2 SSH failures, check the security group, subnet route, public IP or bastion path, OS user, and private key. [AWS CLI EC2 Instance Connect `ssh`](https://docs.aws.amazon.com/cli/latest/reference/ec2-instance-connect/ssh.html)
- For SSM failures, verify that the instance is a managed node, has the SSM Agent and IAM permissions, and can reach Systems Manager endpoints. [AWS CLI SSM `start-session`](https://docs.aws.amazon.com/cli/latest/reference/ssm/start-session.html)
- For ECS Fargate failures, inspect task stopped reasons with `aws ecs describe-tasks` and stream CloudWatch logs with `aws logs tail`. [AWS CLI ECS `run-task`](https://docs.aws.amazon.com/cli/latest/reference/ecs/run-task.html) [AWS CLI CloudWatch Logs `tail`](https://docs.aws.amazon.com/cli/latest/reference/logs/tail.html)
- For EKS failures, check cluster status with `aws eks describe-cluster`, node group status with `aws eks describe-nodegroup`, and Kubernetes events with `kubectl get events`. [AWS CLI EKS `create-cluster`](https://docs.aws.amazon.com/cli/latest/reference/eks/create-cluster.html) [AWS CLI EKS `create-nodegroup`](https://docs.aws.amazon.com/cli/latest/reference/eks/create-nodegroup.html)
- For Batch failures, inspect `statusReason` and container exit details with `aws batch describe-jobs`. [AWS CLI Batch `submit-job`](https://docs.aws.amazon.com/cli/latest/reference/batch/submit-job.html)

## 9. Cleanup checklist

Delete resources in dependency order so higher-level schedulers do not recreate lower-level resources.

```bash
# EC2 instances
aws ec2 terminate-instances --instance-ids "$INSTANCE_ID"

# Auto Scaling group and launch template
aws autoscaling delete-auto-scaling-group \
  --auto-scaling-group-name cli-web-asg \
  --force-delete
aws ec2 delete-launch-template --launch-template-name cli-asg-template

# ECS service and cluster
aws ecs update-service --cluster cli-fargate-demo --service cli-nginx-service --desired-count 0 || true
aws ecs delete-service --cluster cli-fargate-demo --service cli-nginx-service --force || true
aws ecs delete-cluster --cluster cli-fargate-demo || true

# EKS workload, node group, and cluster
kubectl delete service hello-nginx || true
kubectl delete deployment hello-nginx || true
aws eks delete-nodegroup --cluster-name cli-eks-demo --nodegroup-name cli-eks-ng || true
aws eks delete-cluster --name cli-eks-demo || true

# AWS Batch queue and compute environment
aws batch update-job-queue --job-queue cli-batch-queue --state DISABLED || true
aws batch delete-job-queue --job-queue cli-batch-queue || true
aws batch update-compute-environment --compute-environment cli-batch-fargate --state DISABLED || true
aws batch delete-compute-environment --compute-environment cli-batch-fargate || true

# Lambda and App Runner
aws lambda delete-function --function-name cli-hello || true
export APP_RUNNER_SERVICE_ARN=$(aws apprunner list-services \
  --query 'ServiceSummaryList[?ServiceName==`cli-apprunner-hello`].ServiceArn | [0]' \
  --output text)
aws apprunner delete-service --service-arn "$APP_RUNNER_SERVICE_ARN" || true
```

Final verification:

```bash
aws sts get-caller-identity
aws ec2 describe-instances --filters "Name=tag:Project,Values=aws-cli-compute-guide" --output table
aws ecs list-clusters
aws eks list-clusters
aws batch describe-compute-environments
aws lambda list-functions --query 'Functions[?FunctionName==`cli-hello`].FunctionName'
```
