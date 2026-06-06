#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta {

// The first storage engine: keeps JSON documents in an in-memory std::vector.
// No persistence yet — purely in-process, so a node has somewhere to put
// objects while the real engines get built behind StorageEngine.
class JsonStore final : public StorageEngine {
public:
    const char* name() const override { return "json"; }

    void add(nlohmann::json doc) override { docs_.push_back(std::move(doc)); }

    std::size_t size() const override { return docs_.size(); }

    const nlohmann::json& at(std::size_t i) const override { return docs_.at(i); }

    // Engine-specific full view, handy for callers that hold a JsonStore&.
    const std::vector<nlohmann::json>& all() const { return docs_; }

private:
    std::vector<nlohmann::json> docs_;
};

}  // namespace zetta
