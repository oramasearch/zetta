// Propose a run, then approve or reject it (#969; docs/pipeline/run-proposals.md).
//
// The chat agent's tools are read-only by design: a chat can describe the node but never
// change it. So "set up a churn project and tell me which accounts are most at risk",
// asked in a thread, cannot build and run anything by itself. It proposes instead.
// `propose_run` compiles the ask into a Pipeline and records a RunProposal with an impact
// preview, and creates nothing else: no Project, no graph, no run. `approve_run_proposal`
// creates exactly what the preview named and runs it through `run` (the correctness gate
// and the real engine, under the caller's own rights). `reject_run_proposal` is the
// cancel path. This is the review-before-build state of #885 applied to a whole run, and
// the "impact preview with a cancel path" lifecycle invariant in CLAUDE.md.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/agent/agent_store.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run_proposal.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::pipeline::RunProposal;
using zetta::pipeline::RunProposalStatus;
using zetta::pipeline::RunProposalStore;

// The run parameters a proposal carries through to `run`, unchanged.
constexpr const char* kRunPassthrough[] = {"dataset_path", "dataset", "top_k", "model_name",
                                           "holdout_fraction"};

std::string actor_of(const ApiContext& context) {
    return context.acting_key.empty() ? std::string("local") : context.acting_key;
}

std::shared_ptr<zetta::pipeline::Project> resolve_project(ApiContext& context,
                                                          const std::string& name) {
    if (auto project = context.node.find_project(name)) return project;
    return context.node.project_by_id(name);
}

nlohmann::json proposal_id_property() {
    return {{"type", "string"}, {"minLength", 1}, {"description", "the proposal's id"}};
}

// The shape every capability here returns: the stored proposal, plus the two calls that
// decide it while it is still open. The `decide` block is the approval challenge an MCP
// client renders — what to call to say yes, and what to call to say no.
nlohmann::json proposal_view(const RunProposal& proposal) {
    nlohmann::json view;
    view["proposal_id"] = proposal.id;
    view["approval"] = std::string(zetta::pipeline::to_string(proposal.status));
    view["message"] = proposal.message;
    view["preview"] = proposal.preview;
    view["proposed_by"] = proposal.proposed_by;
    view["proposed_at"] = proposal.proposed_at;
    if (!proposal.agent.empty()) view["agent"] = proposal.agent;
    if (proposal.status == RunProposalStatus::Proposed) {
        view["decide"] = {
            {"approve", {{"tool", "approve_run_proposal"}, {"input", {{"proposal_id", proposal.id}}}}},
            {"reject", {{"tool", "reject_run_proposal"}, {"input", {{"proposal_id", proposal.id}}}}}};
    } else {
        view["decided_by"] = proposal.decided_by;
        view["decided_at"] = proposal.decided_at;
    }
    if (!proposal.rejection_reason.empty()) view["rejection_reason"] = proposal.rejection_reason;
    if (!proposal.run_id.empty()) view["run_id"] = proposal.run_id;
    if (!proposal.project_id.empty()) view["project_id"] = proposal.project_id;
    if (!proposal.graph_id.empty()) view["graph_id"] = proposal.graph_id;
    return view;
}

const nlohmann::json& preview_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"description",
         "what approving will do, and nothing more: the Project it creates (or uses), the "
         "Pipeline it stores and its steps, and the dataset the run trains on"},
        {"properties",
         {{"project", {{"type", "string"}}},
          {"creates_project", {{"type", "boolean"}}},
          {"graph", {{"type", "string"}}},
          {"steps", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"template", {{"type", "string"}}},
          {"methodology", {{"type", "string"}}},
          {"shape", {{"type", "string"}}},
          {"dataset",
           {{"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}}}, {"description", {{"type", "string"}}}}}}}}}};
    return schema;
}

const nlohmann::json& proposal_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"proposal_id", "approval", "message", "preview"}},
        {"properties",
         {{"proposal_id", {{"type", "string"}}},
          {"approval", {{"type", "string"}, {"enum", {"proposed", "approved", "rejected"}}}},
          {"message", {{"type", "string"}}},
          {"preview", preview_schema()},
          {"proposed_by", {{"type", "string"}}},
          {"proposed_at", {{"type", "string"}}},
          {"agent", {{"type", "string"}}},
          {"decide",
           {{"type", "object"},
            {"description",
             "present while the proposal is open: the call that approves it and the call "
             "that rejects it"}}},
          {"decided_by", {{"type", "string"}}},
          {"decided_at", {{"type", "string"}}},
          {"rejection_reason", {{"type", "string"}}},
          {"run_id", {{"type", "string"}}},
          {"project_id", {{"type", "string"}}},
          {"graph_id", {{"type", "string"}}}}}};
    return schema;
}

// --- propose_run ---------------------------------------------------------------------

const nlohmann::json& propose_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"message"}},
        {"additionalProperties", false},
        {"properties",
         {{"message",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the plain-language ask, e.g. 'predict which customers will churn'"}}},
          {"agent",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the agent whose chat turn is proposing this, when one is"}}},
          {"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "land the Pipeline on this existing project (name or uuid); when omitted, "
             "approving creates the new project the preview names"}}},
          {"graph_name",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "name for the stored Pipeline (default: from the ask's words)"}}},
          {"dataset_path",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "`database.table` or a dataset directory on the node; the built-in demo "
             "dataset when omitted"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"top_k",
           {{"type", "integer"}, {"minimum", 0}, {"description", "at-risk list cap (0 = all)"}}},
          {"model_name", {{"type", "string"}, {"minLength", 1}}},
          {"holdout_fraction",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"description", "rows held out to measure the model on data it did not train on"}}}}}};
    return schema;
}

// Whether a dataset_path names something this node holds, so a preview never promises a
// dataset that is not there. `database.table` is checked against the catalog, anything
// else against the node's filesystem (the dataset_path grammar, docs/data/dataset-path.md).
std::optional<std::string> missing_dataset(ApiContext& context, const std::string& path) {
    const zetta::data::DatasetPath parsed = zetta::data::parse_dataset_path(path);
    if (parsed.kind == zetta::data::DatasetPathKind::Table) {
        const auto database = context.node.find_database(parsed.database);
        if (!database || !database->find_table(parsed.table)) {
            return "no dataset '" + path + "' on this node — list_datasets shows what is there";
        }
        return std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::exists(parsed.directory, error)) {
        return "no dataset directory '" + path + "' on this node";
    }
    return std::nullopt;
}

ApiResult handle_propose_run(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no run proposals on a standalone node");
    }
    const std::string message = input.at("message").get<std::string>();

    std::string agent_id;
    if (const auto agent = input.find("agent"); agent != input.end()) {
        agent_id = agent->get<std::string>();
        zetta::agent::AgentStore agents(*context.node.replication());
        if (!agents.get(agent_id)) {
            return ApiResult::failure("not_found", "no agent '" + agent_id + "'", "/agent");
        }
    }

    // The same compiler `compile_intent` and `agent_run` use, so a proposal compiles the
    // ask exactly as building it directly would.
    std::string reason;
    std::optional<zetta::pipeline::CompiledIntent> compiled =
        zetta::pipeline::compile_intent(message, &reason);
    if (!compiled) {
        return ApiResult::failure("engine_error", "could not compile a Pipeline from the ask: " + reason,
                                  "/message");
    }
    const std::string shape = std::string(zetta::pipeline::to_string(compiled->shape));

    // Where it lands. A named project must exist now; otherwise the preview names the new
    // project approval will create, and no project exists until then.
    nlohmann::json preview;
    std::string project_name;
    if (const auto named = input.find("project"); named != input.end()) {
        const auto project = resolve_project(context, named->get<std::string>());
        if (!project) {
            return ApiResult::failure("not_found",
                                      "no project '" + named->get<std::string>() + "' (or with that id)",
                                      "/project");
        }
        project_name = project->name();
        preview["creates_project"] = false;
        preview["project_id"] = project->id();
    } else {
        const std::string stem = compiled->name.empty() ? "agent " + shape : compiled->name;
        do {
            project_name = stem + " · " + zetta::util::new_uuid().substr(0, 8);
        } while (context.node.find_project(project_name));
        preview["creates_project"] = true;
    }
    preview["project"] = project_name;

    std::string graph_name = input.value("graph_name", compiled->name);
    if (graph_name.empty()) graph_name = shape + " pipeline";
    if (!preview.at("creates_project").get<bool>()) {
        const auto project = context.node.find_project(project_name);
        if (project && project->find_graph(graph_name)) {
            return ApiResult::failure(
                "conflict",
                "project '" + project_name + "' already has a Pipeline named '" + graph_name +
                    "' — pass graph_name to name this one",
                "/graph_name");
        }
    }
    preview["graph"] = graph_name;
    nlohmann::json steps = nlohmann::json::array();
    for (const std::shared_ptr<zetta::ir::Node>& node : compiled->graph.nodes()) {
        steps.push_back(node->name);
    }
    preview["steps"] = std::move(steps);
    preview["shape"] = shape;
    if (!compiled->template_name.empty()) preview["template"] = compiled->template_name;
    if (!compiled->methodology.empty()) preview["methodology"] = compiled->methodology;

    nlohmann::json run_input = nlohmann::json::object();
    for (const char* key : kRunPassthrough) {
        if (const auto found = input.find(key); found != input.end()) run_input[key] = *found;
    }
    if (const auto path = input.find("dataset_path"); path != input.end()) {
        if (const auto missing = missing_dataset(context, path->get<std::string>())) {
            return ApiResult::failure("invalid_input", *missing, "/dataset_path");
        }
        preview["dataset"] = {{"path", path->get<std::string>()},
                              {"description", "the dataset at " + path->get<std::string>()}};
    } else {
        preview["dataset"] = {{"path", ""}, {"description", "the built-in demo dataset"}};
    }

    RunProposal proposal;
    proposal.id = zetta::util::new_uuid();
    proposal.message = message;
    proposal.agent = agent_id;
    proposal.proposed_by = actor_of(context);
    proposal.proposed_at = zetta::util::now_iso8601();
    proposal.preview = std::move(preview);
    proposal.graph = zetta::ir::to_json(compiled->graph);
    proposal.run_input = std::move(run_input);

    RunProposalStore store(*context.node.replication());
    std::string error;
    const std::optional<RunProposal> stored = store.put(proposal, &error);
    if (!stored) return ApiResult::failure("engine_error", "could not record the proposal: " + error);
    return ApiResult::success(proposal_view(*stored));
}

// --- approve_run_proposal ------------------------------------------------------------

const nlohmann::json& proposal_id_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"proposal_id"}},
        {"additionalProperties", false},
        {"properties", {{"proposal_id", proposal_id_property()}}}};
    return schema;
}

const nlohmann::json& approve_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json value = proposal_output_schema();
        value["required"].push_back("run");
        value["properties"]["project"] = {{"type", "string"}};
        value["properties"]["graph"] = {{"type", "string"}};
        value["properties"]["run"] = {
            {"type", "object"},
            {"description", "the run `run` started (run_id + engine + status) — poll get_run"}};
        return value;
    }();
    return schema;
}

// A proposal that cannot be decided any more, said with what already happened to it.
ApiResult already_decided(const RunProposal& proposal, const char* verb) {
    if (proposal.status == RunProposalStatus::Approved) {
        return ApiResult::failure(
            "conflict",
            "run proposal '" + proposal.id + "' was already approved by " + proposal.decided_by +
                " at " + proposal.decided_at +
                (proposal.run_id.empty() ? std::string() : "; its run is " + proposal.run_id) +
                " — nothing was " + verb + " again",
            "/proposal_id");
    }
    return ApiResult::failure(
        "conflict",
        "run proposal '" + proposal.id + "' was rejected by " + proposal.decided_by + " at " +
            proposal.decided_at + " — propose the run again to build it",
        "/proposal_id");
}

ApiResult unknown_proposal(const std::string& id) {
    return ApiResult::failure("not_found",
                              "no run proposal '" + id + "' — nothing was proposed under that id",
                              "/proposal_id");
}

ApiResult handle_approve_run_proposal(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no run proposals on a standalone node");
    }
    const std::string id = input.at("proposal_id").get<std::string>();
    RunProposalStore store(*context.node.replication());
    std::optional<RunProposal> proposal = store.get(id);
    if (!proposal) return unknown_proposal(id);
    if (proposal->status != RunProposalStatus::Proposed) return already_decided(*proposal, "created");
    if (context.registry == nullptr) {
        return ApiResult::failure("engine_error", "no capability registry for this call");
    }

    std::string reason;
    std::optional<zetta::ir::ExecutionGraph> graph = zetta::ir::from_json(proposal->graph, &reason);
    if (!graph) {
        return ApiResult::failure("engine_error", "the proposal's Pipeline does not parse: " + reason);
    }

    // Exactly what the preview said: the project it named (created now, or the existing one
    // it named by id), and the Pipeline under the name it showed. Anything that would make
    // the result differ from the preview refuses instead.
    const nlohmann::json& preview = proposal->preview;
    const std::string project_name = preview.value("project", std::string());
    const std::string graph_name = preview.value("graph", std::string());
    std::shared_ptr<zetta::pipeline::Project> project;
    if (preview.value("creates_project", true)) {
        if (context.node.find_project(project_name)) {
            return ApiResult::failure(
                "conflict",
                "the preview said approving creates project '" + project_name +
                    "', and a project with that name now exists — nothing was created; "
                    "propose the run again",
                "/proposal_id");
        }
        project = context.node.create_project(project_name, actor_of(context));
        if (!project) {
            return ApiResult::failure("engine_error", "could not create project '" + project_name + "'");
        }
    } else {
        project = context.node.project_by_id(preview.value("project_id", std::string()));
        if (!project) {
            return ApiResult::failure("conflict",
                                      "the project '" + project_name +
                                          "' the preview named no longer exists — nothing was created",
                                      "/proposal_id");
        }
        if (project->find_graph(graph_name)) {
            return ApiResult::failure("conflict",
                                      "project '" + project->name() + "' now has a Pipeline named '" +
                                          graph_name + "' — nothing was created; propose the run again",
                                      "/proposal_id");
        }
    }
    const std::shared_ptr<zetta::pipeline::ProjectGraph> stored =
        project->create_graph(graph_name, std::move(*graph));
    if (!stored) {
        return ApiResult::failure("engine_error", "could not store the Pipeline '" + graph_name + "'");
    }
    const std::string decided_at = zetta::util::now_iso8601();
    // Approving the proposal IS approving the plan, recorded against the person who said
    // yes — the same review-before-build state `approve_plan` sets (#885).
    project->approve_graph(stored->name(), actor_of(context), decided_at);

    proposal->status = RunProposalStatus::Approved;
    proposal->decided_by = actor_of(context);
    proposal->decided_at = decided_at;
    proposal->project_id = project->id();
    proposal->graph_id = stored->id();

    // Run it through `run` itself, under the caller's own rights: the correctness gate, the
    // real engine, the audit row. Approval grants nothing the caller could not do directly.
    nlohmann::json run_input = proposal->run_input;
    run_input["project"] = project->name();
    run_input["graph"] = stored->name();
    const ApiResult ran = context.registry->call("run", run_input, context);
    if (ran.ok) proposal->run_id = ran.value.value("run_id", std::string());

    std::string error;
    if (!store.put(*proposal, &error)) {
        return ApiResult::failure("engine_error", "could not record the approval: " + error);
    }
    if (!ran.ok) {
        ApiResult failed = ran;
        failed.error.message = "approved: created project '" + project->name() + "' and Pipeline '" +
                               stored->name() + "', but the run did not start: " +
                               ran.error.message + " — fix that, then call run";
        return failed;
    }
    nlohmann::json output = proposal_view(*proposal);
    output["project"] = project->name();
    output["graph"] = stored->name();
    output["run"] = ran.value;
    return ApiResult::success(std::move(output));
}

// --- reject_run_proposal -------------------------------------------------------------

const nlohmann::json& reject_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"proposal_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"proposal_id", proposal_id_property()},
          {"reason", {{"type", "string"}, {"description", "why, in the caller's words"}}}}}};
    return schema;
}

ApiResult handle_reject_run_proposal(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no run proposals on a standalone node");
    }
    const std::string id = input.at("proposal_id").get<std::string>();
    RunProposalStore store(*context.node.replication());
    std::optional<RunProposal> proposal = store.get(id);
    if (!proposal) return unknown_proposal(id);
    // Rejecting twice changes nothing and says so by returning the same record.
    if (proposal->status == RunProposalStatus::Rejected) return ApiResult::success(proposal_view(*proposal));
    if (proposal->status == RunProposalStatus::Approved) return already_decided(*proposal, "undone");
    proposal->status = RunProposalStatus::Rejected;
    proposal->decided_by = actor_of(context);
    proposal->decided_at = zetta::util::now_iso8601();
    proposal->rejection_reason = input.value("reason", std::string());
    std::string error;
    const std::optional<RunProposal> stored = store.put(*proposal, &error);
    if (!stored) return ApiResult::failure("engine_error", "could not record the rejection: " + error);
    return ApiResult::success(proposal_view(*stored));
}

// --- get_run_proposal / list_run_proposals -------------------------------------------

ApiResult handle_get_run_proposal(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no run proposals on a standalone node");
    }
    const std::string id = input.at("proposal_id").get<std::string>();
    const std::optional<RunProposal> proposal = RunProposalStore(*context.node.replication()).get(id);
    if (!proposal) return unknown_proposal(id);
    return ApiResult::success(proposal_view(*proposal));
}

const nlohmann::json& list_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"status", {{"type", "string"}, {"enum", {"proposed", "approved", "rejected"}}}}}}};
    return schema;
}

const nlohmann::json& list_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"proposals"}},
        {"properties", {{"proposals", {{"type", "array"}, {"items", proposal_output_schema()}}}}}};
    return schema;
}

ApiResult handle_list_run_proposals(const nlohmann::json& input, ApiContext& context) {
    nlohmann::json proposals = nlohmann::json::array();
    if (context.node.replication() != nullptr) {
        const std::string status = input.value("status", std::string());
        for (const RunProposal& proposal : RunProposalStore(*context.node.replication()).list()) {
            if (!status.empty() && zetta::pipeline::to_string(proposal.status) != status) continue;
            proposals.push_back(proposal_view(proposal));
        }
    }
    return ApiResult::success({{"proposals", std::move(proposals)}});
}

}  // namespace

void register_run_proposal_capabilities(Registry& registry) {
    registry.add(Capability{
        "propose_run",
        "Propose a run",
        "Compile a plain-language ask into a Pipeline and record it as a proposal with an "
        "impact preview: the Project approving would create, the Pipeline's steps, and the "
        "dataset it would train on. Creates nothing else — no Project, no Pipeline, no run — "
        "until approve_run_proposal. This is how a chat agent asks to build and run something.",
        propose_run_schema(),
        proposal_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "propose_run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_propose_run});
    registry.add(Capability{
        "approve_run_proposal",
        "Approve a run proposal",
        "Say yes to a proposed run: create exactly the Project and Pipeline its preview named, "
        "approve the plan under your name, and run it through the correctness gate and the "
        "real engine with your own rights. Returns the run handle (poll get_run). Refuses a "
        "proposal that does not exist or was already decided.",
        proposal_id_schema(),
        approve_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "approve_run_proposal"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_approve_run_proposal});
    registry.add(Capability{
        "reject_run_proposal",
        "Reject a run proposal",
        "Say no to a proposed run. Nothing is created. Idempotent; refuses a proposal that "
        "was already approved.",
        reject_schema(),
        proposal_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "reject_run_proposal"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_reject_run_proposal});
    registry.add(Capability{
        "get_run_proposal",
        "Read a run proposal",
        "Read one run proposal: its impact preview, whether it is proposed, approved or "
        "rejected, and what approving it created. Read-only.",
        proposal_id_schema(),
        proposal_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_run_proposal});
    registry.add(Capability{
        "list_run_proposals",
        "List run proposals",
        "List run proposals, newest first, optionally only those still proposed, approved or "
        "rejected. Read-only.",
        list_schema(),
        list_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_run_proposals});
}

}  // namespace zetta::api
