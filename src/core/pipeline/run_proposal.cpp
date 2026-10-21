#include "zetta/pipeline/run_proposal.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "../catalog.hpp"
#include "zetta/cluster/replication.hpp"

namespace zetta::pipeline {

std::string_view to_string(RunProposalStatus status) {
    switch (status) {
        case RunProposalStatus::Proposed:
            return "proposed";
        case RunProposalStatus::Approved:
            return "approved";
        case RunProposalStatus::Rejected:
            return "rejected";
    }
    return "proposed";
}

std::optional<RunProposalStatus> run_proposal_status_from_string(std::string_view text) {
    if (text == "proposed") return RunProposalStatus::Proposed;
    if (text == "approved") return RunProposalStatus::Approved;
    if (text == "rejected") return RunProposalStatus::Rejected;
    return std::nullopt;
}

nlohmann::json to_json(const RunProposal& proposal) {
    return {{"id", proposal.id},
            {"status", std::string(to_string(proposal.status))},
            {"message", proposal.message},
            {"agent", proposal.agent},
            {"proposed_by", proposal.proposed_by},
            {"proposed_at", proposal.proposed_at},
            {"preview", proposal.preview},
            {"graph", proposal.graph},
            {"run_input", proposal.run_input},
            {"decided_by", proposal.decided_by},
            {"decided_at", proposal.decided_at},
            {"rejection_reason", proposal.rejection_reason},
            {"project_id", proposal.project_id},
            {"graph_id", proposal.graph_id},
            {"run_id", proposal.run_id}};
}

std::optional<RunProposal> run_proposal_from_json(const nlohmann::json& value,
                                                  std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<RunProposal> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("run proposal: not an object");

    RunProposal proposal;
    const auto text = [&value](const char* key, std::string& out) {
        const auto found = value.find(key);
        if (found == value.end() || found->is_null()) return true;
        if (!found->is_string()) return false;
        out = found->get<std::string>();
        return true;
    };
    const auto object = [&value](const char* key, nlohmann::json& out) {
        const auto found = value.find(key);
        if (found == value.end() || found->is_null()) return true;
        if (!found->is_object()) return false;
        out = *found;
        return true;
    };
    for (const auto& [key, field] :
         {std::pair<const char*, std::string*>{"id", &proposal.id},
          {"message", &proposal.message},
          {"agent", &proposal.agent},
          {"proposed_by", &proposal.proposed_by},
          {"proposed_at", &proposal.proposed_at},
          {"decided_by", &proposal.decided_by},
          {"decided_at", &proposal.decided_at},
          {"rejection_reason", &proposal.rejection_reason},
          {"project_id", &proposal.project_id},
          {"graph_id", &proposal.graph_id},
          {"run_id", &proposal.run_id}}) {
        if (!text(key, *field)) return fail(std::string("run proposal: /") + key + " is not a string");
    }
    for (const auto& [key, field] :
         {std::pair<const char*, nlohmann::json*>{"preview", &proposal.preview},
          {"graph", &proposal.graph},
          {"run_input", &proposal.run_input}}) {
        if (!object(key, *field)) return fail(std::string("run proposal: /") + key + " is not an object");
    }
    if (proposal.id.empty()) return fail("run proposal: /id is empty");
    if (proposal.message.empty()) return fail("run proposal: /message is empty");

    std::string status = "proposed";
    if (!text("status", status)) return fail("run proposal: /status is not a string");
    const std::optional<RunProposalStatus> parsed = run_proposal_status_from_string(status);
    if (!parsed) return fail("run proposal: /status '" + status + "' is not proposed, approved or rejected");
    proposal.status = *parsed;
    return proposal;
}

RunProposalStore::RunProposalStore(zetta::cluster::ReplicationManager& replication)
    : replication_(replication) {}

std::optional<RunProposal> RunProposalStore::put(const RunProposal& proposal,
                                                 std::string* error) {
    std::optional<RunProposal> validated = run_proposal_from_json(to_json(proposal), error);
    if (!validated) return std::nullopt;
    replication_.set(catalog::run_proposal_key(validated->id), to_json(*validated));
    return validated;
}

// Reads take the shared immutable view, never a deep copy (the AgentStore pattern, #1227).
std::optional<RunProposal> RunProposalStore::get(const std::string& id) const {
    if (id.empty()) return std::nullopt;
    const std::shared_ptr<const nlohmann::json> document = replication_.shared_snapshot();
    const auto found = document->find(catalog::run_proposal_key(id));
    if (found == document->end()) return std::nullopt;
    return run_proposal_from_json(*found, nullptr);
}

std::vector<RunProposal> RunProposalStore::list() const {
    std::vector<RunProposal> proposals;
    const std::shared_ptr<const nlohmann::json> document = replication_.shared_snapshot();
    catalog::for_each_with_prefix(
        *document, catalog::kRunProposalPrefix,
        [&proposals](const std::string&, const nlohmann::json& value) {
            if (std::optional<RunProposal> proposal = run_proposal_from_json(value, nullptr)) {
                proposals.push_back(*std::move(proposal));
            }
        });
    std::sort(proposals.begin(), proposals.end(),
              [](const RunProposal& left, const RunProposal& right) {
                  if (left.proposed_at != right.proposed_at) {
                      return left.proposed_at > right.proposed_at;
                  }
                  return left.id < right.id;
              });
    return proposals;
}

}  // namespace zetta::pipeline
