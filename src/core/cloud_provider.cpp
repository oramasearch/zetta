#include "zetta/cloud_provider.hpp"

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
            .inspect_commands = {"ssh <host> -- uname -a",
                                 "ssh <host> -- nproc"},
        },
        CloudProvider{
            .id = "kubernetes",
            .name = "Kubernetes",
            .category = "Kubernetes",
            .summary = "A customer-managed Kubernetes cluster (any distribution).",
            .cli = "kubectl",
            .config_paths = {"~/.kube/config"},
            .inspect_commands = {"kubectl config current-context",
                                 "kubectl cluster-info",
                                 "kubectl get nodes"},
        },
        CloudProvider{
            .id = "aws",
            .name = "Amazon Web Services",
            .category = "Managed cloud",
            .summary = "EC2 / EKS compute in your AWS account.",
            .cli = "aws",
            .config_paths = {"~/.aws/config", "~/.aws/credentials"},
            .inspect_commands = {"aws sts get-caller-identity",
                                 "aws configure list",
                                 "aws ec2 describe-regions"},
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
            .inspect_commands = {"gcloud config list", "gcloud auth list",
                                 "gcloud compute regions list"},
        },
        CloudProvider{
            .id = "azure",
            .name = "Microsoft Azure",
            .category = "Managed cloud",
            .summary = "Virtual Machines / AKS in your Azure subscription.",
            .cli = "az",
            .config_paths = {"~/.azure/config", "~/.azure/azureProfile.json"},
            .inspect_commands = {"az account show",
                                 "az account list-locations"},
        },
        CloudProvider{
            .id = "oci",
            .name = "Oracle Cloud Infrastructure",
            .category = "Managed cloud",
            .summary = "Compute in your OCI tenancy.",
            .cli = "oci",
            .config_paths = {"~/.oci/config"},
            .inspect_commands = {"oci iam region list", "oci os ns get"},
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
