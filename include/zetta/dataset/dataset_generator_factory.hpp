#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "zetta/dataset/dataset_generator.hpp"

// The factory of native dataset generators (#873). One place registers every DatasetGenerator
// (the base agent's corpus, Auto-Suggest, future kinds); callers create one by kind. The
// `generate_dataset` capability + the GUI/web dataset view both drive this factory, so a new
// generator shows up everywhere by registering here — nothing else changes (the seam rule).
namespace zetta::dataset {

class DatasetGeneratorFactory {
public:
    // The process-wide factory of built-in generators (registered in the .cpp).
    static const DatasetGeneratorFactory& builtin();

    // The generator for `kind` ("base-agent" / "auto-suggest"), or an empty handle if unknown.
    std::shared_ptr<const DatasetGenerator> create(std::string_view kind) const;

    // Every registered generator, and their kind slugs — for the capability + the UI listing.
    const std::vector<std::shared_ptr<const DatasetGenerator>>& generators() const {
        return generators_;
    }
    std::vector<std::string> kinds() const;

    explicit DatasetGeneratorFactory(
        std::vector<std::shared_ptr<const DatasetGenerator>> generators)
        : generators_(std::move(generators)) {}

private:
    std::vector<std::shared_ptr<const DatasetGenerator>> generators_;
};

}  // namespace zetta::dataset
