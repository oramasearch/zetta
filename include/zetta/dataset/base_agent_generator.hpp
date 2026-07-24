#pragma once

#include <string>

#include "zetta/dataset/dataset_generator.hpp"

// The native generator for the base agent's intent→pipeline corpus (#873 / #555). It synthesizes
// admission-gated tasks grounded in the solutions-discovery plan — each a use case × methodology
// (a solution) with its correct pipeline's facts as the reference and degenerate pipelines as
// negative controls — and attaches the reference ACTION ({message, tool_calls, execution_graph})
// via the solution-graph bridge. Grows toward full-plan coverage (all 24 use cases, 21 methods,
// 11 components, 12 solutions, the data supply chain, autonomy); see the coverage checklist.
namespace zetta::dataset {

class BaseAgentDatasetGenerator : public DatasetGenerator {
public:
    std::string kind() const override { return "base-agent"; }
    std::string title() const override { return "Base agent corpus"; }
    std::string description() const override {
        return "The Orama Studio auto-AI agent's intent→pipeline training corpus: gated tasks "
               "grounded in the solutions plan, each with its correct pipeline + discriminating "
               "negative controls, in the {message, tool_calls, execution_graph} action shape.";
    }
    GeneratedDataset generate(const DatasetGenSpec& spec) const override;
};

}  // namespace zetta::dataset
