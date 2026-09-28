#pragma once

#include "zetta/data/featurize_plan.hpp"
#include "zetta/ml/methodology.hpp"

namespace zetta::pipeline {
class Run;
}

namespace zetta::ml {
class Model;

// Shared run preflight and frozen feature provenance (#1062).
// Native featurizers supply their actual transformations in options.native_entries;
// numeric table trainers receive the plan's accepted source columns in dataset.
// See docs/data/featurization.md and docs/pipeline/run-report.md.
data::FeaturePlan plan_run_features(RunRequest& request, const std::string& methodology,
                                    data::ResolvedDataset& dataset,
                                    data::FeaturePlanOptions options = {});

std::string feature_plan_error(const data::FeaturePlan& plan);
api::ApiResult run_dataset_failure(RunRequest& request, const std::string& methodology,
                                  const data::ResolvedDataset& dataset);
void record_run_features(Node& node, const std::string& run_id, const data::FeaturePlan& plan);
nlohmann::json run_feature_report(const pipeline::Run& run, const Model& model);

} // namespace zetta::ml
