#include "zetta/cloud/provisioner_detect.hpp"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/command_runner.hpp"

namespace zetta {
namespace {

// Trim ASCII whitespace both ends — CLI -o tsv output carries a trailing newline.
std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Read one `az config get <key>` default (e.g. "defaults.location"). `az` returns
// a non-zero exit when the key is unset; on success it prints a JSON object with a
// "value" field. Returns the value, or std::nullopt when unset / unavailable.
std::optional<std::string> az_default(const CommandRunner& run,
                                      const std::string& key) {
    const CommandResult result = run({"az", "config", "get", key, "-o", "json"});
    if (result.exit_code != 0) return std::nullopt;
    const nlohmann::json parsed =
        nlohmann::json::parse(result.stdout_text, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object() || !parsed.contains("value") ||
        !parsed["value"].is_string()) {
        return std::nullopt;
    }
    std::string value = parsed["value"].get<std::string>();
    if (value.empty()) return std::nullopt;
    return value;
}

// Parse a JSON array of strings (an `az … --query "[…].name" -o json` result),
// skipping non-strings. Empty when the output isn't a JSON string array.
std::vector<std::string> parse_string_array(const std::string& json_text) {
    std::vector<std::string> values;
    const nlohmann::json parsed =
        nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_array()) return values;
    for (const nlohmann::json& element : parsed) {
        if (element.is_string()) values.push_back(element.get<std::string>());
    }
    return values;
}

// The Azure regions the subscription can deploy to (`az account list-locations`,
// physical regions only, sorted). Empty on any CLI error — the GUI then falls back
// to a free-text region field.
std::vector<std::string> azure_regions(const CommandRunner& run) {
    // `az account list-locations` operates on the active subscription and does NOT
    // accept `--subscription` (it errors "unrecognized arguments"). Physical regions
    // only, sorted.
    const CommandResult result =
        run({"az", "account", "list-locations", "--query",
             "sort([?metadata.regionType=='Physical'].name)", "-o", "json"});
    if (result.exit_code != 0) return {};
    return parse_string_array(result.stdout_text);
}

}  // namespace

std::optional<ProvisionerConfig> detect_azure_provisioner(
    const CommandRunner& run) {
    // The local `az` CLI's own identity — the same call the Cloud Inspector makes.
    // A non-zero exit means `az` is absent or not logged in: not detected, no error.
    const CommandResult account = run({"az", "account", "show", "-o", "json"});
    if (account.exit_code != 0) return std::nullopt;
    const nlohmann::json acct =
        nlohmann::json::parse(account.stdout_text, nullptr, /*allow_exceptions=*/false);
    if (!acct.is_object() || !acct.contains("id") || !acct["id"].is_string()) {
        return std::nullopt;
    }

    AzureConfig config;
    config.subscription_id = acct["id"].get<std::string>();
    if (acct.contains("tenantId") && acct["tenantId"].is_string()) {
        config.tenant_id = acct["tenantId"].get<std::string>();
    }
    // Resource group + region: the operator's configured `az` defaults when set,
    // else a stable burst group + a default region. Both are non-secret targeting,
    // visible in system.zetta_provisioners; an operator who wants different ones
    // sets `az configure --defaults group=… location=…`.
    config.resource_group = az_default(run, "defaults.group").value_or("zetta-burst");
    config.location = az_default(run, "defaults.location").value_or("eastus");

    ProvisionerConfig detected;
    detected.target = "azure";
    detected.cli = "az";
    detected.config = config.to_json();
    detected.summary = "subscription " + config.subscription_id + " · " +
                       config.resource_group + " · " + config.location;
    // The regions the Add-nodes Region dropdown offers (best-effort — an empty list
    // means the GUI falls back to a free-text field).
    detected.available_regions = azure_regions(run);
    return detected;
}

std::vector<std::string> azure_vm_sizes(const std::string& subscription,
                                        const std::string& location,
                                        CommandRunner runner) {
    const CommandRunner run = runner ? std::move(runner) : default_command_runner();
    // Sizes with NO subscription/policy restriction in the region — the deployable
    // options. (Live capacity isn't reflected here; the control plane's SKU
    // fallback handles a capacity shortage at create time.)
    const CommandResult result = run(
        {"az", "vm", "list-skus", "--subscription", subscription, "--location",
         location, "--resource-type", "virtualMachines", "--query",
         "sort([?restrictions==`[]`].name)", "-o", "json"});
    if (result.exit_code != 0) return {};
    return parse_string_array(result.stdout_text);
}

std::vector<ProvisionerConfig> detect_provisioner_configs(CommandRunner runner) {
    const CommandRunner run = runner ? std::move(runner) : default_command_runner();

    std::vector<ProvisionerConfig> detected;
    // Azure is the reference detector; AWS / GCP / OCI / Kubernetes / bare-metal
    // follow the same shape (their CLI's `whoami`/`config` → their XConfig) and slot
    // in here as they land. Each is independent and best-effort: one CLI being
    // absent never blocks the others.
    if (std::optional<ProvisionerConfig> azure = detect_azure_provisioner(run)) {
        detected.push_back(std::move(*azure));
    }
    return detected;
}

}  // namespace zetta
