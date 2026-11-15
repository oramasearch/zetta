// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "zetta/dataset/dataset_generator.hpp"

// The native generator for the Auto-Suggest corpus (#879 / #554) — the SECOND model: it reads a
// customer's data state (tables + columns present) and suggests zero-to-many use cases, each a
// prompt that feeds the intent agent. Trained the blender-rl way: a data state constructed to
// support a KNOWN set of use cases (correct suggestions) with DISTRACTOR use cases whose data is
// ABSENT and must NOT be suggested — the discrimination signal. Modeled as admission-gated cases
// (task = the data state, required = suggest the present + NOT the absent), so it rides the same
// gate as the base-agent corpus and a per-customer fork (#853) can grow its own from real data.
namespace zetta::dataset {

class AutoSuggestDatasetGenerator : public DatasetGenerator {
public:
    std::string kind() const override { return "auto-suggest"; }
    std::string title() const override { return "Auto-Suggest corpus"; }
    std::string description() const override {
        return "The Auto-Suggest model's training corpus: a data state → the use cases to "
               "suggest (present) and to withhold (absent, distractors) — gated so suggestions "
               "and distractors are disjoint and every sample discriminates.";
    }
    GeneratedDataset generate(const DatasetGenSpec& spec) const override;
};

}  // namespace zetta::dataset
