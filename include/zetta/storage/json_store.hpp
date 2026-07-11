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
    void update(const std::string& id, nlohmann::json doc) override {
        if (const std::size_t i = index_of(id); i != kMissing) {
            docs_[i] = std::move(doc);
        }
    }
    void remove(const std::string& id) override {
        const std::size_t i = index_of(id);
        if (i == kMissing) return;
        docs_.erase(docs_.begin() + static_cast<std::ptrdiff_t>(i));
        ids_.erase(ids_.begin() + static_cast<std::ptrdiff_t>(i));
        id_index_.erase(id);
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
    static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);
    // Index of the row with this id, or kMissing. Linear over ids_ (gated by the
    // set) — fine for the small tables this in-memory engine backs.
    std::size_t index_of(const std::string& id) const {
        if (!id_index_.contains(id)) return kMissing;
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            if (ids_[i] == id) return i;
        }
        return kMissing;
    }

    std::vector<nlohmann::json> docs_;
    std::vector<std::string> ids_;             // parallel to docs_
    std::unordered_set<std::string> id_index_;  // for contains()
};

}  // namespace zetta::storage