#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/background.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cloud/add_nodes.hpp"
#include "zetta/cloud/cloud_inspector.hpp"
#include "zetta/cloud/cloud_provider.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/cloud/provisioner_config.hpp"
#include "zetta/cloud/provisioner_detect.hpp"
#include "zetta/governance/consent.hpp"
#include "zetta/node.hpp"

// The cloud-touching capabilities + the consent tools (#621;
// docs/api/consent.md). Every cloud tool here is `requires_consent`: the first
// call NEVER executes — Registry::call issues a one-time consent challenge
// naming exactly what would be read/run (the Cloud Inspector grammar), and
// only a distinctly-privileged approval (an approver key of a different
// principal via `approve_consent`, or the headed app's approval surface)
// executes the frozen action, exactly once. The `local` target is the one
// input-aware exception: it touches no cloud, so its describer waives the
// challenge — the same gate shape the headed GUI applies.
//
// The slow halves (provider CLI shell-outs, cloud provisions) run OFF the
// owner thread on a worker (the AsyncCaseRun discipline), and their results
// are applied back to the Node — and the challenge's outcome recorded — on
// the owner thread when the embedding prunes its BackgroundActions.
namespace zetta::api {

namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

// Runs one cloud action's slow, Node-free half on a worker thread. The owner
// keeps the object alive through BackgroundActions until done(); the
// destructor joins (on the owner thread, at prune or teardown). The mutex is
// the contained cross-thread handoff for the finished payload, mirroring the
// headed app's background-op structs (#624).
class AsyncCloudAction {
public:
    explicit AsyncCloudAction(std::function<nlohmann::json()> work) {
        worker_ = std::thread([this, work = std::move(work), audit_origin = zetta::governance::capture_audit_origin()] {
            // The worker's audit rows name whoever launched it (#1327).
            const zetta::governance::AuditOriginScope audit_scope{audit_origin};
            nlohmann::json output = work();
            {
                std::lock_guard<std::mutex> hold(mutex_);
                result_ = std::move(output);
            }
            done_.store(true, std::memory_order_release);
        });
    }
    ~AsyncCloudAction() {
        if (worker_.joinable()) worker_.join();
    }

    AsyncCloudAction(const AsyncCloudAction&) = delete;
    AsyncCloudAction& operator=(const AsyncCloudAction&) = delete;

    bool done() const { return done_.load(std::memory_order_acquire); }
    nlohmann::json result() const {
        std::lock_guard<std::mutex> hold(mutex_);
        return result_;
    }

private:
    std::thread worker_;
    std::atomic<bool> done_{false};
    mutable std::mutex mutex_;
    nlohmann::json result_;
};

// Launch `work` off-thread and arrange for `apply` to run on the owner thread
// when it finishes (inside BackgroundActions::prune). When the execution was
// consent-approved, the challenge's outcome is recorded at the same moment —
// `work` marks failure by putting `"ok": false` (+ `"detail"`) in its output.
// Returns `immediate_output` to the caller right away (the job discipline:
// never block a call on cloud work).
ApiResult launch_cloud_action(ApiContext& context,
                              std::function<nlohmann::json()> work,
                              std::function<void(Node&, const nlohmann::json&)> apply,
                              nlohmann::json immediate_output) {
    if (!context.background) {
        return ApiResult::failure(
            "engine_error",
            "cloud actions run on a background worker and this embedding "
            "wired no background action owner");
    }
    auto action = std::make_shared<AsyncCloudAction>(std::move(work));
    Node* node = &context.node;
    const std::string challenge_id = context.consent_challenge_id;
    context.consent_outcome_deferred = !challenge_id.empty();
    context.background->adopt(
        action, [action = action.get(), node, apply = std::move(apply),
                 challenge_id] {
            if (!action->done()) return false;
            const nlohmann::json outcome = action->result();
            if (apply) apply(*node, outcome);
            if (!challenge_id.empty()) {
                const bool ok = outcome.value("ok", true);
                node->consent_ledger().record_outcome(
                    challenge_id, ok, outcome,
                    ok ? std::string{} : outcome.value("detail", std::string{}));
            }
            return true;
        });
    return ApiResult::success(std::move(immediate_output));
}

// ----- cloud_inspect ---------------------------------------------------------

nlohmann::json cloud_inspect_schema() {
    nlohmann::json provider_ids = nlohmann::json::array();
    for (const zetta::cloud::CloudProvider& provider :
         zetta::cloud::CloudProviderCatalog::builtin().providers()) {
        provider_ids.push_back(provider.id);
    }
    return {{"type", "object"},
            {"required", nlohmann::json::array({"provider"})},
            {"additionalProperties", false},
            {"properties",
             {{"provider",
               {{"type", "string"},
                {"enum", provider_ids},
                {"description",
                 "The catalog provider to inspect (read-only identity / "
                 "inventory commands)"}}}}}};
}

const nlohmann::json& cloud_inspect_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"status", "provider", "read_back"}},
        {"additionalProperties", false},
        {"properties",
         {{"status",
           {{"type", "string"},
            {"description",
             "'inspecting' — the CLI reads run on a background worker"}}},
          {"provider", {{"type", "string"}}},
          {"read_back",
           {{"type", "string"},
            {"description", "where the result lands (system.cloud_status)"}}}}}};
    return schema;
}

std::optional<zetta::governance::ConsentAction> describe_cloud_inspect(
    const nlohmann::json& input, ApiContext&) {
    const auto* provider = zetta::cloud::CloudProviderCatalog::builtin().find(
        input.value("provider", std::string{}));
    if (!provider || !provider->requires_consent()) return std::nullopt;
    zetta::governance::ConsentAction action;
    action.summary = "Inspect " + provider->name + " (read-only): run the " +
                     (provider->cli.empty() ? std::string{"provider"}
                                            : provider->cli) +
                     " CLI's identity/inventory commands. zetta never reads "
                     "or prints the named config files itself.";
    action.reads = provider->config_paths;
    for (const zetta::cloud::InspectCommand& command :
         provider->inspect_commands) {
        action.runs.push_back(command.display);
    }
    return action;
}

ApiResult handle_cloud_inspect(const nlohmann::json& input,
                               ApiContext& context) {
    const std::string provider_id = input.at("provider").get<std::string>();
    const auto* provider =
        zetta::cloud::CloudProviderCatalog::builtin().find(provider_id);
    if (!provider) {
        return ApiResult::failure(
            "not_found", "no cloud provider with id '" + provider_id + "'");
    }
    return launch_cloud_action(
        context,
        // Off-thread: the provider CLI shell-outs (the one real cloud read),
        // already consent-approved by the time this handler runs.
        [provider_copy = *provider] {
            const zetta::cloud::CloudInspectReport report =
                zetta::cloud::CloudInspector{}.inspect(provider_copy);
            nlohmann::json outcome = {{"ok", true},
                                      {"report", zetta::cloud::to_json(report)}};
            if (!report.ran) outcome["detail"] = report.detail;
            return outcome;
        },
        // Owner thread: cache the report on the node (also audits
        // cloud_inspected) so system.cloud_status shows it.
        [](Node& node, const nlohmann::json& outcome) {
            if (auto report = zetta::cloud::parse_cloud_inspect_report(
                    outcome.value("report", nlohmann::json::object()))) {
                node.record_cloud_inspection(std::move(*report));
            }
        },
        nlohmann::json{{"status", "inspecting"},
                       {"provider", provider_id},
                       {"read_back",
                        "system.cloud_status (and the consent challenge via "
                        "list_consent_requests when approval started this)"}});
}

// ----- detect_provisioners ---------------------------------------------------

nlohmann::json detect_provisioners_schema() {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", nlohmann::json::object()}};
}

const nlohmann::json& detect_provisioners_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"status", "read_back"}},
        {"additionalProperties", false},
        {"properties",
         {{"status",
           {{"type", "string"},
            {"description",
             "'detecting' — the CLI queries run on a background worker"}}},
          {"read_back",
           {{"type", "string"},
            {"description", "where the result lands (system.provisioners)"}}}}}};
    return schema;
}

std::optional<zetta::governance::ConsentAction> describe_detect_provisioners(
    const nlohmann::json&, ApiContext&) {
    zetta::governance::ConsentAction action;
    action.summary =
        "Detect which cloud-burst targets this node can drive by asking the "
        "operator's own local CLIs for their active deployment context "
        "(read-only; no credentials are read or stored).";
    // The Azure reference detector's exact commands (provisioner_detect.cpp);
    // new detectors extend this list as they land.
    action.runs = {"az account show -o json",
                   "az config get defaults.group -o json",
                   "az config get defaults.location -o json",
                   "az account list-locations -o json"};
    return action;
}

ApiResult handle_detect_provisioners(const nlohmann::json&,
                                     ApiContext& context) {
    // The worker hands the typed configs to the owner-thread apply through
    // shared state (the done flag's release/acquire orders the write).
    auto detected = std::make_shared<std::vector<zetta::cloud::ProvisionerConfig>>();
    return launch_cloud_action(
        context,
        [detected] {
            *detected = zetta::cloud::detect_provisioner_configs();
            nlohmann::json rows = nlohmann::json::array();
            for (const zetta::cloud::ProvisionerConfig& config : *detected) {
                rows.push_back({{"target", config.target},
                                {"cli", config.cli},
                                {"summary", config.summary}});
            }
            return nlohmann::json{{"ok", true}, {"provisioners", rows}};
        },
        [detected](Node& node, const nlohmann::json&) {
            for (zetta::cloud::ProvisionerConfig& config : *detected) {
                node.record_provisioner_config(std::move(config));
            }
        },
        nlohmann::json{{"status", "detecting"},
                       {"read_back", "system.provisioners"}});
}

// ----- add_nodes -------------------------------------------------------------

nlohmann::json add_nodes_schema() {
    nlohmann::json target_ids = nlohmann::json::array();
    for (const zetta::cloud::CloudProvider& provider :
         zetta::cloud::CloudProviderCatalog::builtin().providers()) {
        target_ids.push_back(provider.id);
    }
    return {{"type", "object"},
            {"required", nlohmann::json::array({"target", "count"})},
            {"additionalProperties", false},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"enum", target_ids},
                {"description",
                 "Where the nodes come up: 'local' (given hardware, no "
                 "cloud) or a cloud target with a wired provisioner"}}},
              {"count",
               {{"type", "integer"},
                {"minimum", 1},
                {"description", "How many permanent nodes to add"}}},
              {"region",
               {{"type", "string"},
                {"description",
                 "Optional placement region for the next provision"}}},
              {"vm_size",
               {{"type", "string"},
                {"description",
                 "Optional VM size / instance type for the next provision"}}}}}};
}

const nlohmann::json& add_nodes_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"status", "target", "count", "read_back"}},
        {"additionalProperties", false},
        {"properties",
         {{"status",
           {{"type", "string"},
            {"description",
             "'provisioning' — the provision runs on a background worker"}}},
          {"target", {{"type", "string"}}},
          {"count", {{"type", "integer"}}},
          {"read_back",
           {{"type", "string"},
            {"description",
             "where the result lands (system.nodes for membership)"}}}}}};
    return schema;
}

std::optional<zetta::governance::ConsentAction> describe_add_nodes(
    const nlohmann::json& input, ApiContext&) {
    const std::string target = input.value("target", std::string{});
    const auto* provider =
        zetta::cloud::CloudProviderCatalog::builtin().find(target);
    // The given-hardware target brings up no new machines and touches no
    // cloud — the GUI runs it on a plain click, so remote parity waives the
    // challenge too.
    if (!provider || !provider->requires_consent()) return std::nullopt;
    const auto count = input.value("count", std::size_t{0});
    const std::string plural = count == 1 ? " billable node" : " billable nodes";
    zetta::governance::ConsentAction action;
    action.summary = "Run the " + provider->cli + " CLI to create " +
                     std::to_string(count) + plural + " on " + provider->name +
                     ". This is a write and it incurs cost.";
    if (const std::string region = input.value("region", std::string{});
        !region.empty()) {
        action.summary += " Placement region: " + region + ".";
    }
    if (const std::string vm_size = input.value("vm_size", std::string{});
        !vm_size.empty()) {
        action.summary += " VM size: " + vm_size + ".";
    }
    action.reads = provider->config_paths;
    action.runs = {provider->cli + " … create " + std::to_string(count) +
                   " node(s) via the " + provider->name +
                   " control plane, each auto-joining this cluster"};
    return action;
}

ApiResult handle_add_nodes(const nlohmann::json& input, ApiContext& context) {
    const std::string target = input.at("target").get<std::string>();
    const std::size_t count = input.at("count").get<std::size_t>();
    if (!context.provisioners) {
        return ApiResult::failure(
            "engine_error",
            "no provisioner targets are wired in this embedding; nothing can "
            "be provisioned here");
    }
    zetta::cloud::Provisioner* provider = context.provisioners(target);
    if (!provider) {
        return ApiResult::failure(
            "engine_error",
            "no provisioner is wired for target '" + target +
                "' (no deployment context was detected from this node's local "
                "CLIs — see system.provisioners)");
    }
    // Steer the next provision's placement before launching, like the headed
    // Add-nodes control; empty strings leave a dimension unchanged.
    const std::string region = input.value("region", std::string{});
    const std::string vm_size = input.value("vm_size", std::string{});
    if (!region.empty() || !vm_size.empty()) {
        provider->set_placement(region, vm_size);
    }

    zetta::cloud::AddNodesRequest request;
    request.count = count;
    auto outcome = std::make_shared<zetta::cloud::AddNodesResult>();
    return launch_cloud_action(
        context,
        // Off-thread: the provision (a cloud CLI drive can take minutes).
        // `provider` outlives the server per the resolver contract.
        [provider, request, outcome] {
            *outcome = zetta::cloud::add_nodes(*provider, request);
            nlohmann::json members = nlohmann::json::array();
            for (const zetta::cloud::ProvisionedNode& member :
                 outcome->members) {
                members.push_back(member.to_json());
            }
            return nlohmann::json{{"ok", outcome->satisfied},
                                  {"members", members},
                                  {"satisfied", outcome->satisfied},
                                  {"detail", outcome->detail}};
        },
        // Owner thread: adopt the new permanent members + rebalance
        // partitions onto them (#519).
        [outcome](Node& node, const nlohmann::json&) {
            zetta::cloud::apply_added_nodes(node, *outcome);
        },
        nlohmann::json{{"status", "provisioning"},
                       {"target", target},
                       {"count", count},
                       {"read_back",
                        "system.nodes for membership (and the consent "
                        "challenge via list_consent_requests when approval "
                        "started this)"}});
}

// ----- the consent tools -----------------------------------------------------

nlohmann::json list_consent_requests_schema() {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", nlohmann::json::object()}};
}

const nlohmann::json& list_consent_requests_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"consent_requests"}},
        {"additionalProperties", false},
        {"properties",
         {{"consent_requests",
           {{"type", "array"},
            {"items", zetta::governance::consent_challenge_schema()}}}}}};
    return schema;
}

ApiResult handle_list_consent_requests(const nlohmann::json&,
                                       ApiContext& context) {
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& challenge : context.node.consent_ledger().all()) {
        rows.push_back(zetta::governance::to_json(*challenge));
    }
    return ApiResult::success({{"consent_requests", std::move(rows)}});
}

nlohmann::json consent_decision_schema(bool with_reason) {
    nlohmann::json properties = {
        {"challenge_id",
         {{"type", "string"},
          {"minLength", 1},
          {"description", "The one-time consent challenge id being decided"}}}};
    if (with_reason) {
        properties["reason"] = {{"type", "string"},
                                {"description", "Why the request is refused"}};
    }
    return {{"type", "object"},
            {"required", nlohmann::json::array({"challenge_id"})},
            {"additionalProperties", false},
            {"properties", std::move(properties)}};
}

const nlohmann::json& approve_consent_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"challenge_id", "capability", "status", "result"}},
        {"additionalProperties", false},
        {"properties",
         {{"challenge_id", {{"type", "string"}}},
          {"capability",
           {{"type", "string"},
            {"description", "the capability the approved challenge names"}}},
          {"status",
           {{"type", "string"},
            {"enum", {"executing", "executed"}},
            {"description",
             "'executing' when the named action continues on a background "
             "worker (read back via list_consent_requests)"}}},
          {"result",
           {{"description",
             "the executed capability's own success value (its output "
             "schema)"}}}}}};
    return schema;
}

const nlohmann::json& deny_consent_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"challenge_id", "status"}},
        {"additionalProperties", false},
        {"properties",
         {{"challenge_id", {{"type", "string"}}},
          {"status", {{"type", "string"}, {"enum", {"denied"}}}}}}};
    return schema;
}

}  // namespace

void register_cloud_capabilities(Registry& registry) {
    registry.add(Capability{
        "cloud_inspect",
        "Inspect a cloud provider",
        "Consent-gated: inspect a cloud provider's identity/regions with its "
        "own CLI (read-only). First call returns a consent challenge; only a "
        "distinctly-privileged approval executes it.",
        cloud_inspect_schema(),
        cloud_inspect_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "cloud_inspect"},
        /*mutates=*/true,
        /*requires_consent=*/true,
        handle_cloud_inspect,
        describe_cloud_inspect});

    registry.add(Capability{
        "detect_provisioners",
        "Detect cloud provisioners",
        "Consent-gated: re-detect which cloud targets this node can drive by "
        "querying the local CLIs' deployment context (read-only). First call "
        "returns a consent challenge.",
        detect_provisioners_schema(),
        detect_provisioners_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "detect_provisioners"},
        /*mutates=*/true,
        /*requires_consent=*/true,
        handle_detect_provisioners,
        describe_detect_provisioners});

    registry.add(Capability{
        "add_nodes",
        "Add cluster nodes",
        "Add permanent nodes to this cluster via a wired provisioner. Cloud "
        "targets are consent-gated (billable): the first call returns a "
        "consent challenge; 'local' executes directly and touches no cloud.",
        add_nodes_schema(),
        add_nodes_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "add_nodes"},
        /*mutates=*/true,
        /*requires_consent=*/true,
        handle_add_nodes,
        describe_add_nodes});

    registry.add(Capability{
        "list_consent_requests",
        "List consent requests",
        "Read every consent challenge on this node's ledger — pending, "
        "decided, and executed (with results). No side effects.",
        list_consent_requests_schema(),
        list_consent_requests_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_consent_requests});

    registry.add(Capability{
        "approve_consent",
        "Approve a consent request",
        "Approve one pending consent challenge and execute exactly the named "
        "action once. Requires a named approver key of a DIFFERENT principal "
        "than the requester — a key can never approve its own cloud action.",
        consent_decision_schema(/*with_reason=*/false),
        approve_consent_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "approve_consent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        [&registry](const nlohmann::json& input, ApiContext& context) {
            return registry.decide_consent(
                input.at("challenge_id").get<std::string>(), /*approve=*/true,
                {}, context, Registry::ConsentSurface::ApproverKey);
        }});

    registry.add(Capability{
        "deny_consent",
        "Deny a consent request",
        "Refuse one pending consent challenge; the named action never runs. "
        "The requesting key may withdraw its own challenge.",
        consent_decision_schema(/*with_reason=*/true),
        deny_consent_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "deny_consent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        [&registry](const nlohmann::json& input, ApiContext& context) {
            return registry.decide_consent(
                input.at("challenge_id").get<std::string>(), /*approve=*/false,
                input.value("reason", std::string{}), context,
                Registry::ConsentSurface::ApproverKey);
        }});
}

}  // namespace zetta::api
