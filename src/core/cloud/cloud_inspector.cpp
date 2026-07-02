#include "zetta/cloud/cloud_inspector.hpp"

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/util/timestamp.hpp"

namespace zetta::cloud {
namespace {

// The CloudInspectReport JSON Schema (draft-07) — zetta's own contract for the
// inspect result. The `$defs` additionally describe the small slices of each
// vendor CLI's JSON output that the inspector validates at the point of
// extraction. Mirrors schemas/cloud_inspect_report.schema.json; this embedded
// copy is the one enforced.
const nlohmann::json& report_schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.CloudInspectReport",
      "type": "object",
      "required": ["provider_id", "ran", "reachable", "authenticated"],
      "additionalProperties": false,
      "properties": {
        "provider_id":   { "type": "string" },
        "provider_name": { "type": "string" },
        "ran":           { "type": "boolean" },
        "reachable":     { "type": "boolean" },
        "authenticated": { "type": "boolean" },
        "identity":      { "type": "string" },
        "locations":     { "type": "array", "items": { "type": "string" } },
        "resources":     { "type": "array", "items": { "type": "string" } },
        "commands": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["display", "ok"],
            "additionalProperties": false,
            "properties": {
              "display":   { "type": "string" },
              "exit_code": { "type": "integer" },
              "ok":        { "type": "boolean" },
              "output":    { "type": "string" },
              "error":     { "type": "string" }
            }
          }
        },
        "detail":     { "type": "string" },
        "checked_at": { "type": "string" }
      },
      "$defs": {
        "aws_identity": {
          "type": "object",
          "required": ["Account", "Arn"],
          "properties": {
            "Account": { "type": "string" },
            "Arn":     { "type": "string" },
            "UserId":  { "type": "string" }
          }
        },
        "aws_regions": {
          "type": "object",
          "required": ["Regions"],
          "properties": {
            "Regions": {
              "type": "array",
              "items": {
                "type": "object",
                "required": ["RegionName"],
                "properties": { "RegionName": { "type": "string" } }
              }
            }
          }
        },
        "gcp_config": {
          "type": "object",
          "properties": {
            "core": {
              "type": "object",
              "properties": {
                "account": { "type": "string" },
                "project": { "type": "string" }
              }
            }
          }
        },
        "gcp_regions": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["name"],
            "properties": { "name": { "type": "string" } }
          }
        },
        "azure_account": {
          "type": "object",
          "required": ["id"],
          "properties": {
            "id":       { "type": "string" },
            "name":     { "type": "string" },
            "tenantId": { "type": "string" },
            "user": {
              "type": "object",
              "properties": { "name": { "type": "string" } }
            }
          }
        },
        "azure_locations": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["name"],
            "properties": {
              "name":        { "type": "string" },
              "displayName": { "type": "string" }
            }
          }
        },
        "oci_regions": {
          "type": "object",
          "required": ["data"],
          "properties": {
            "data": {
              "type": "array",
              "items": {
                "type": "object",
                "required": ["name"],
                "properties": { "name": { "type": "string" } }
              }
            }
          }
        },
        "oci_namespace": {
          "type": "object",
          "required": ["data"],
          "properties": { "data": { "type": "string" } }
        },
        "k8s_config": {
          "type": "object",
          "properties": {
            "current-context": { "type": "string" },
            "clusters": {
              "type": "array",
              "items": {
                "type": "object",
                "properties": {
                  "name": { "type": "string" },
                  "cluster": {
                    "type": "object",
                    "properties": { "server": { "type": "string" } }
                  }
                }
              }
            }
          }
        },
        "k8s_nodes": {
          "type": "object",
          "required": ["items"],
          "properties": {
            "items": {
              "type": "array",
              "items": {
                "type": "object",
                "properties": {
                  "metadata": {
                    "type": "object",
                    "properties": { "name": { "type": "string" } }
                  }
                }
              }
            }
          }
        }
      }
    })JSONSCHEMA");
    return schema;
}

// Collects the first validation failure (the error-handler overload of
// validate() keeps failures from crossing the validator boundary as exceptions —
// the same pattern as run.cpp).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// The report validator, compiled once.
nlohmann::json_schema::json_validator& report_validator() {
    static nlohmann::json_schema::json_validator validator(report_schema_document());
    return validator;
}

// One validator per `$def` (each def is self-contained — no cross-refs), built
// lazily. Used to validate a vendor CLI's JSON slice before any field is read.
nlohmann::json_schema::json_validator& vendor_validator(const std::string& def_key) {
    static std::map<std::string, nlohmann::json_schema::json_validator> validators = [] {
        std::map<std::string, nlohmann::json_schema::json_validator> built;
        const nlohmann::json& defs = report_schema_document().at("$defs");
        for (auto entry = defs.begin(); entry != defs.end(); ++entry) {
            nlohmann::json root = entry.value();
            root["$schema"] = "http://json-schema.org/draft-07/schema#";
            built.emplace(entry.key(),
                          nlohmann::json_schema::json_validator(std::move(root)));
        }
        return built;
    }();
    return validators.at(def_key);
}

bool validate_vendor(const std::string& def_key, const nlohmann::json& document,
                     std::string* error) {
    collecting_error_handler handler;
    vendor_validator(def_key).validate(document, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

// Cap captured output so a long region list / node dump doesn't bloat the report.
std::string truncate_output(const std::string& text) {
    constexpr std::size_t kMax = 1500;
    if (text.size() <= kMax) return text;
    return text.substr(0, kMax) + "\n… (truncated)";
}

// A command that was actually run — keeps the full (untruncated) output for
// parsing, alongside the display string used to match it.
struct RanCommand {
    std::string display;
    std::string full_output;
    bool ok = false;
};

// Set the error on the report command whose display contains `signature`.
void annotate_command(CloudInspectReport& report, std::string_view signature,
                      const std::string& error) {
    for (CloudInspectCommand& command : report.commands) {
        if (command.display.find(signature) != std::string::npos) {
            if (command.error.empty()) command.error = error;
            return;
        }
    }
}

// Find the ran command whose display contains `signature`, parse its output as
// JSON, and validate it against `def_key`. On any failure (command didn't
// succeed, non-JSON, schema mismatch) annotate the report command and return
// nullopt — the inspect degrades gracefully, never throws.
std::optional<nlohmann::json> inspected(CloudInspectReport& report,
                                        const std::vector<RanCommand>& ran,
                                        std::string_view signature,
                                        const std::string& def_key) {
    const RanCommand* match = nullptr;
    for (const RanCommand& candidate : ran) {
        if (candidate.display.find(signature) != std::string::npos) {
            match = &candidate;
            break;
        }
    }
    if (match == nullptr || !match->ok) return std::nullopt;  // already recorded

    nlohmann::json document =
        nlohmann::json::parse(match->full_output, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        annotate_command(report, signature, "output was not valid JSON");
        return std::nullopt;
    }
    std::string error;
    if (!validate_vendor(def_key, document, &error)) {
        annotate_command(report, signature, "unexpected output shape: " + error);
        return std::nullopt;
    }
    return document;
}

void interpret_aws(CloudInspectReport& report, const std::vector<RanCommand>& ran) {
    if (auto identity = inspected(report, ran, "get-caller-identity", "aws_identity")) {
        const std::string account = identity->value("Account", std::string{});
        const std::string arn = identity->value("Arn", std::string{});
        report.identity = "account " + account + (arn.empty() ? "" : " · " + arn);
        report.authenticated = !account.empty();
    }
    if (auto regions = inspected(report, ran, "describe-regions", "aws_regions")) {
        for (const nlohmann::json& region : regions->at("Regions")) {
            report.locations.push_back(region.value("RegionName", std::string{}));
        }
    }
}

void interpret_gcp(CloudInspectReport& report, const std::vector<RanCommand>& ran) {
    if (auto config = inspected(report, ran, "config list", "gcp_config")) {
        const nlohmann::json core = config->value("core", nlohmann::json::object());
        const std::string account = core.value("account", std::string{});
        const std::string project = core.value("project", std::string{});
        if (!account.empty() || !project.empty()) {
            report.identity =
                account + (project.empty() ? "" : " · project " + project);
            report.authenticated = !account.empty();
        }
    }
    if (auto regions = inspected(report, ran, "regions list", "gcp_regions")) {
        for (const nlohmann::json& region : *regions) {
            report.locations.push_back(region.value("name", std::string{}));
        }
    }
}

void interpret_azure(CloudInspectReport& report, const std::vector<RanCommand>& ran) {
    if (auto account = inspected(report, ran, "account show", "azure_account")) {
        const nlohmann::json user = account->value("user", nlohmann::json::object());
        const std::string user_name = user.value("name", std::string{});
        const std::string subscription = account->value("name", std::string{});
        report.identity =
            (user_name.empty() ? std::string{} : user_name) +
            (subscription.empty() ? "" : " · subscription " + subscription);
        report.authenticated = true;
    }
    if (auto locations = inspected(report, ran, "list-locations", "azure_locations")) {
        for (const nlohmann::json& location : *locations) {
            const std::string display = location.value("displayName", std::string{});
            report.locations.push_back(
                display.empty() ? location.value("name", std::string{}) : display);
        }
    }
}

void interpret_oci(CloudInspectReport& report, const std::vector<RanCommand>& ran) {
    if (auto namespace_json = inspected(report, ran, "os ns get", "oci_namespace")) {
        const std::string object_namespace =
            namespace_json->value("data", std::string{});
        report.identity = "namespace " + object_namespace;
        report.authenticated = !object_namespace.empty();
    }
    if (auto regions = inspected(report, ran, "region list", "oci_regions")) {
        for (const nlohmann::json& region : regions->at("data")) {
            report.locations.push_back(region.value("name", std::string{}));
        }
    }
}

void interpret_kubernetes(CloudInspectReport& report,
                          const std::vector<RanCommand>& ran) {
    if (auto config = inspected(report, ran, "config view", "k8s_config")) {
        const std::string context = config->value("current-context", std::string{});
        report.identity = context.empty() ? std::string{} : "context " + context;
        report.authenticated = !context.empty();
    }
    if (auto nodes = inspected(report, ran, "get nodes", "k8s_nodes")) {
        for (const nlohmann::json& node : nodes->at("items")) {
            const nlohmann::json metadata =
                node.value("metadata", nlohmann::json::object());
            const std::string name = metadata.value("name", std::string{});
            if (!name.empty()) report.resources.push_back(name);
        }
    }
}

void interpret(const std::string& provider_id, CloudInspectReport& report,
               const std::vector<RanCommand>& ran) {
    if (provider_id == "aws") return interpret_aws(report, ran);
    if (provider_id == "gcp") return interpret_gcp(report, ran);
    if (provider_id == "azure") return interpret_azure(report, ran);
    if (provider_id == "oci") return interpret_oci(report, ran);
    if (provider_id == "kubernetes") return interpret_kubernetes(report, ran);
    // Unknown/non-runnable provider: nothing to interpret.
}

}  // namespace

CloudInspector::CloudInspector(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

CloudInspectReport CloudInspector::inspect(const CloudProvider& provider) const {
    CloudInspectReport report;
    report.provider_id = provider.id;
    report.provider_name = provider.name;
    report.checked_at = zetta::util::now_iso8601();

    if (!provider.runnable()) {
        // Described but not yet runnable (bare-metal's ssh needs a host). Report
        // honestly that nothing ran — no cloud was touched.
        report.ran = false;
        report.detail =
            "inspecting " + provider.name +
            " from zetta isn't available yet (an inspect needs a configured host).";
        return report;
    }
    report.ran = true;

    std::vector<RanCommand> ran;
    for (const InspectCommand& command : provider.inspect_commands) {
        if (command.argv.empty()) continue;  // display-only entry, not runnable

        const CommandResult result = runner_(command.argv);
        CloudInspectCommand outcome;
        outcome.display = command.display;
        outcome.exit_code = result.exit_code;
        outcome.ok = result.exit_code == 0;
        outcome.output = truncate_output(result.stdout_text);
        if (!outcome.ok) {
            outcome.error =
                result.exit_code == 127
                    ? "the " + provider.cli + " CLI was not found (exit 127)"
                    : "command failed (exit " + std::to_string(result.exit_code) + ")";
        } else {
            report.reachable = true;
        }
        report.commands.push_back(std::move(outcome));
        ran.push_back(
            {command.display, result.stdout_text, result.exit_code == 0});
    }

    interpret(provider.id, report, ran);

    if (!report.reachable) {
        report.detail = "could not reach " + provider.name + " — is the " +
                        provider.cli + " CLI installed and configured?";
    } else if (!report.authenticated && report.detail.empty()) {
        report.detail = "reachable, but no identity was returned (not logged in?)";
    }
    return report;
}

nlohmann::json to_json(const CloudInspectReport& report) {
    nlohmann::json commands = nlohmann::json::array();
    for (const CloudInspectCommand& command : report.commands) {
        commands.push_back({{"display", command.display},
                            {"exit_code", command.exit_code},
                            {"ok", command.ok},
                            {"output", command.output},
                            {"error", command.error}});
    }
    return nlohmann::json{{"provider_id", report.provider_id},
                          {"provider_name", report.provider_name},
                          {"ran", report.ran},
                          {"reachable", report.reachable},
                          {"authenticated", report.authenticated},
                          {"identity", report.identity},
                          {"locations", report.locations},
                          {"resources", report.resources},
                          {"commands", std::move(commands)},
                          {"detail", report.detail},
                          {"checked_at", report.checked_at}};
}

const nlohmann::json& cloud_inspect_report_schema() {
    return report_schema_document();
}

bool validate_cloud_inspect_report(const nlohmann::json& value,
                                   std::string* error) {
    collecting_error_handler handler;
    report_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<CloudInspectReport> parse_cloud_inspect_report(
    const nlohmann::json& value, std::string* error) {
    if (!validate_cloud_inspect_report(value, error)) return std::nullopt;

    CloudInspectReport report;
    report.provider_id = value.value("provider_id", std::string{});
    report.provider_name = value.value("provider_name", std::string{});
    report.ran = value.value("ran", false);
    report.reachable = value.value("reachable", false);
    report.authenticated = value.value("authenticated", false);
    report.identity = value.value("identity", std::string{});
    report.locations =
        value.value("locations", std::vector<std::string>{});
    report.resources =
        value.value("resources", std::vector<std::string>{});
    report.detail = value.value("detail", std::string{});
    report.checked_at = value.value("checked_at", std::string{});
    if (value.contains("commands")) {
        for (const nlohmann::json& command : value.at("commands")) {
            CloudInspectCommand outcome;
            outcome.display = command.value("display", std::string{});
            outcome.exit_code = command.value("exit_code", 0);
            outcome.ok = command.value("ok", false);
            outcome.output = command.value("output", std::string{});
            outcome.error = command.value("error", std::string{});
            report.commands.push_back(std::move(outcome));
        }
    }
    return report;
}

}  // namespace zetta::cloud