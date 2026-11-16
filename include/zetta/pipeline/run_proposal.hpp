// SPDX-License-Identifier: MIT

#pragma once

// A run an agent PROPOSED and nobody has approved yet (#969).
//
// The chat agent may describe the node but never change it, so when a person asks
// it to build and run something the agent cannot just do it. It proposes instead:
// the engine compiles the ask into a Pipeline and records a RunProposal carrying an
// impact preview (the Project it would create, the Pipeline's steps, the dataset it
// would train on). Nothing else exists yet: no Project, no graph, no run. Approving
// the proposal creates exactly what the preview named and runs it; rejecting it is
// the cancel path. docs/pipeline/run-proposals.md.
//
// Proposals are replicated state (`run_proposal:<uuid>` CRDT keys), read straight
// from the document like agents are, so they persist and replicate with no
// parallel store.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::cluster {
class ReplicationManager;
}

namespace zetta::pipeline {

enum class RunProposalStatus { Proposed, Approved, Rejected };

std::string_view to_string(RunProposalStatus status);
std::optional<RunProposalStatus> run_proposal_status_from_string(std::string_view text);

struct RunProposal {
    std::string id;
    RunProposalStatus status = RunProposalStatus::Proposed;
    // The plain-language ask, verbatim.
    std::string message;
    // The agent whose chat turn proposed it; empty when a caller proposed directly.
    std::string agent;
    std::string proposed_by;
    std::string proposed_at;
    // What approving will do, in the caller's terms: project, creates_project,
    // graph, steps, template, methodology, dataset. Approval produces exactly this.
    nlohmann::json preview = nlohmann::json::object();
    // The compiled Pipeline, stored whole so approval lands this graph rather than
    // recompiling the ask (which could compile differently on a later build).
    nlohmann::json graph = nlohmann::json::object();
    // The run parameters approval passes to `run` (dataset_path, top_k, …).
    nlohmann::json run_input = nlohmann::json::object();
    // Set by approve or reject.
    std::string decided_by;
    std::string decided_at;
    std::string rejection_reason;
    // Set by approve: what it created.
    std::string project_id;
    std::string graph_id;
    std::string run_id;
};

nlohmann::json to_json(const RunProposal& proposal);
// Non-throwing boundary parser; std::nullopt (+ *error naming the field) when malformed.
std::optional<RunProposal> run_proposal_from_json(const nlohmann::json& value,
                                                  std::string* error);

// CRUD over the replicated `run_proposal:<uuid>` entities. Pure: it stores records as
// given; ids, timestamps and the decision rules are the caller's. Non-throwing.
class RunProposalStore {
public:
    explicit RunProposalStore(zetta::cluster::ReplicationManager& replication);

    // Create or replace (captures `run_proposal:<id>`). std::nullopt (+ *error) when the
    // record is malformed.
    std::optional<RunProposal> put(const RunProposal& proposal, std::string* error);
    std::optional<RunProposal> get(const std::string& id) const;
    // All proposals, newest first (proposed_at, then id).
    std::vector<RunProposal> list() const;

private:
    zetta::cluster::ReplicationManager& replication_;
};

}  // namespace zetta::pipeline
