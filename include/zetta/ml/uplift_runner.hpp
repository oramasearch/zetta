#pragma once

#include <memory>
#include <string>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/uplift.hpp"

namespace zetta {
class Node;
}

namespace zetta::pipeline {
class Run;
}

namespace zetta::ml {

struct UpliftRunOptions {
  UpliftParams params;
  std::string treatment_column;
  std::string outcome_column;
  std::string model_name = "uplift_model";
  std::string project;
  std::string graph;
  bool coordinate = true;
};

struct UpliftRunResult {
  std::shared_ptr<pipeline::Run> run;
  UpliftResult uplift;
  std::string model_id;
  std::string treatment_effects_table;
  std::string qini_table;
  std::string error;

  bool ok() const noexcept { return error.empty() && uplift.ok(); }
};

// Run M-6 against a dataset already resolved through the shared dataset-path
// contract. The function reads owned partitions, trains through the distributed
// uplift job, and writes the model, treatment effects, Qini curve, and lineage.
UpliftRunResult run_uplift_on_table(Node &node,
                                    const data::ResolvedDataset &dataset,
                                    const UpliftRunOptions &options);

} // namespace zetta::ml
