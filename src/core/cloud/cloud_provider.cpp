#include "zetta/cloud/cloud_provider.hpp"

namespace zetta {

const CloudProviderCatalog& CloudProviderCatalog::builtin() {
    // Built once, on first use. Pure metadata — see cloud_provider.hpp: nothing
    // here reads a config file or calls a cloud. `config_paths` /
    // `inspect_commands` only *describe* what a (later, consent-gated) real
    // inspect would touch, so the consent gate can name it up front.
    //
    // Ordered local → on-prem → Kubernetes → managed clouds. Each cloud /
    // Kubernetes / on-prem entry lists the credential/config files an inspect
    // would read and the **read-only** identity/inventory commands it would run
    // (never provision/deploy/teardown). Local lists neither — it is this node,
    // so it requires no consent.
    static const CloudProviderCatalog catalog{{
        CloudProvider{
            .id = "local",
            .name = "Local",
            .category = "Local",
            .summary = "This machine — the running zetta node. No external "
                       "access; nothing to authorize.",
            .cli = {},
            .config_paths = {},
            .inspect_commands = {},
        },
        CloudProvider{
            .id = "bare-metal",
            .name = "Bare metal (on-prem)",
            .category = "On-prem",
            .summary = "On-prem hosts you own, reached over SSH.",
            .cli = "ssh",
            .config_paths = {"~/.ssh/config", "~/.ssh/known_hosts"},
            // Display-only (empty argv): an inspect needs a concrete host, which
            // isn't configured yet, so bare-metal requires consent but is not
            // runnable. The gate still shows the shape an inspect would take.
            .inspect_commands = {{.display = "ssh <host> -- uname -a"},
                                 {.display = "ssh <host> -- nproc"}},
        },
        CloudProvider{
            .id = "kubernetes",
            .name = "Kubernetes",
            .category = "Kubernetes",
            .summary = "A customer-managed Kubernetes cluster (any distribution).",
            .cli = "kubectl",
            .config_paths = {"~/.kube/config"},
            .inspect_commands =
                {{.display = "kubectl config view --minify -o json",
                  .argv = {"kubectl", "config", "view", "--minify", "-o",
                           "json"}},
                 {.display = "kubectl get nodes -o json",
                  .argv = {"kubectl", "get", "nodes", "-o", "json"}}},
        },
        CloudProvider{
            .id = "aws",
            .name = "Amazon Web Services",
            .category = "Managed cloud",
            .summary = "EC2 / EKS compute in your AWS account.",
            .cli = "aws",
            .config_paths = {"~/.aws/config", "~/.aws/credentials"},
            .inspect_commands =
                {{.display = "aws sts get-caller-identity --output json",
                  .argv = {"aws", "sts", "get-caller-identity", "--output",
                           "json"}},
                 {.display = "aws ec2 describe-regions --output json",
                  .argv = {"aws", "ec2", "describe-regions", "--output",
                           "json"}}},
        },
        CloudProvider{
            .id = "gcp",
            .name = "Google Cloud Platform",
            .category = "Managed cloud",
            .summary = "Compute Engine / GKE in your GCP project.",
            .cli = "gcloud",
            .config_paths =
                {"~/.config/gcloud/configurations/config_default",
                 "~/.config/gcloud/application_default_credentials.json"},
            .inspect_commands =
                {{.display = "gcloud config list --format=json",
                  .argv = {"gcloud", "config", "list", "--format=json"}},
                 {.display = "gcloud compute regions list --format=json",
                  .argv = {"gcloud", "compute", "regions", "list",
                           "--format=json"}}},
        },
        CloudProvider{
            .id = "azure",
            .name = "Microsoft Azure",
            .category = "Managed cloud",
            .summary = "Virtual Machines / AKS in your Azure subscription.",
            .cli = "az",
            .config_paths = {"~/.azure/config", "~/.azure/azureProfile.json"},
            .inspect_commands =
                {{.display = "az account show --output json",
                  .argv = {"az", "account", "show", "--output", "json"}},
                 {.display = "az account list-locations --output json",
                  .argv = {"az", "account", "list-locations", "--output",
                           "json"}}},
        },
        CloudProvider{
            .id = "oci",
            .name = "Oracle Cloud Infrastructure",
            .category = "Managed cloud",
            .summary = "Compute in your OCI tenancy.",
            .cli = "oci",
            .config_paths = {"~/.oci/config"},
            .inspect_commands =
                {{.display = "oci iam region list --output json",
                  .argv = {"oci", "iam", "region", "list", "--output", "json"}},
                 {.display = "oci os ns get --output json",
                  .argv = {"oci", "os", "ns", "get", "--output", "json"}}},
        },
    }};
    return catalog;
}

const CloudProvider* CloudProviderCatalog::find(const std::string& id) const {
    for (const CloudProvider& provider : providers_) {
        if (provider.id == id) return &provider;
    }
    return nullptr;
}

}  // namespace zetta
