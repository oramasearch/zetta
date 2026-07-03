#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta::storage {

// An in-memory storage engine: keeps rows (and their ids) in std::vectors with
// no persistence. Kept for a future `:memory:` mode; the on-disk JsonlStore is
// what tables use by default.
class JsonStore final : public StorageEngine {
public:
    const char* name() const override { return "json"; }

    void add(std::string id, nlohmann::json doc) override {
        ids_.push_back(id);
        id_index_.insert(std::move(id));
        docs_.push_back(std::move(doc));
    }
    bool contains(const std::string& id) const override {
        return id_index_.contains(id);
    }
    std::size_t size() const override { return docs_.size(); }
    const nlohmann::json& at(std::size_t i) const override { return docs_.at(i); }
    const std::string& id_at(std::size_t i) const override { return ids_.at(i); }

    // Engine-specific full view, handy for callers that hold a JsonStore&.
    const std::vector<nlohmann::json>& all() const { return docs_; }

private:
    std::vector<nlohmann::json> docs_;
    std::vector<std::string> ids_;             // parallel to docs_
    std::unordered_set<std::string> id_index_;  // for contains()
};

}  // namespace zetta::storage