// SPDX-License-Identifier: MIT

#include "zetta/dataset/dataset_generator_factory.hpp"

#include <memory>
#include <string>
#include <vector>

#include "zetta/dataset/auto_suggest_generator.hpp"
#include "zetta/dataset/authored_generator.hpp"
#include "zetta/dataset/base_agent_generator.hpp"

namespace zetta::dataset {

const DatasetGeneratorFactory& DatasetGeneratorFactory::builtin() {
    // Register every native dataset generator here — a new kind shows up in the capability + the
    // GUI/web listing by adding one line (the seam rule).
    static const DatasetGeneratorFactory factory({
        std::make_shared<const AuthoredDatasetGenerator>(),
        std::make_shared<const BaseAgentDatasetGenerator>(),
        std::make_shared<const AutoSuggestDatasetGenerator>(),
    });
    return factory;
}

std::shared_ptr<const DatasetGenerator> DatasetGeneratorFactory::create(
    std::string_view kind) const {
    for (const std::shared_ptr<const DatasetGenerator>& generator : generators_) {
        if (generator->kind() == kind) return generator;
    }
    return nullptr;
}

std::vector<std::string> DatasetGeneratorFactory::kinds() const {
    std::vector<std::string> result;
    result.reserve(generators_.size());
    for (const std::shared_ptr<const DatasetGenerator>& generator : generators_) {
        result.push_back(generator->kind());
    }
    return result;
}

}  // namespace zetta::dataset
