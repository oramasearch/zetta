// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/next_best_action_job.hpp"

namespace zetta {
class Node;
}

namespace zetta::pipeline {
class Run;
}

namespace zetta::ml {

struct NextBestActionTableParams {
  NextBestActionParams model;
  std::string action_column = "action_id";
  std::string value_column = "action_value";
  std::string cost_column = "action_cost";
  std::string eligibility_column = "eligible";
  std::string contacts_column = "contacts";
  std::string contact_cap_column = "contact_cap";
  std::string interaction_count_column = "interaction_count";
  std::string interaction_time_column = "interaction_time";
  std::string logging_propensity_column = "logging_propensity";
  std::string model_name = "next_best_action_model";
  std::string round_tag = "ml.next_best_action";
  nlohmann::json dataset_facts = nlohmann::json::object();
};

struct NextBestActionTableResult : DistributedNextBestActionResult {
  std::string recommendation_table;
  std::string policy_report_table;
  std::string model_id;
  std::uint64_t partitions_read = 0;
  std::uint64_t bytes_read = 0;
};

// Load only this member's owned partitions from a resolved dataset, run the
// distributed trainer, and materialize queryable recommendation/policy tables
// plus a Model on the initiating member. The supplied Run receives output
// references and source lineage. Participants must call concurrently.
NextBestActionTableResult run_next_best_action_on_table(
    Node &node, const data::ResolvedDataset &dataset,
    const NextBestActionTableParams &params, pipeline::Run &run,
    std::chrono::milliseconds timeout = kDefaultNextBestActionTimeout);

// Peer half of a coordinated registry run. It reads this member's owned
// partitions and enters every collective but creates no model or output table.
NextBestActionTableResult run_next_best_action_table_participant(
    Node &node, const data::ResolvedDataset &dataset,
    const NextBestActionTableParams &params, const std::string &project_id,
    const std::string &graph_id, const std::string &run_id,
    std::chrono::milliseconds timeout = kDefaultNextBestActionTimeout);

} // namespace zetta::ml
