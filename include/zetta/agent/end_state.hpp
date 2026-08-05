#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

// The observable end-state of an agent rollout, projected as a set of named facts
// — the thing a deterministic grader checks against a task's success criterion
// (#555, the RLVR reward primitive; see docs/agent/README.md). A "fact" is a key
// with a JSON value: what the world looks like after the agent's tool calls ran in
// the sandbox. No model and no judgement live here — an EndState is plain data, so
// the grader that scores it is a pure, reproducible function.
namespace zetta::agent {

class EndState {
public:
    EndState() = default;
    explicit EndState(std::map<std::string, nlohmann::json> facts)
        : facts_(std::move(facts)) {}

    // Add or overwrite a fact.
    void set(std::string key, nlohmann::json value) {
        facts_.insert_or_assign(std::move(key), std::move(value));
    }

    // Is `key` present at all, regardless of value?
    bool has(const std::string& key) const { return facts_.contains(key); }

    // Is `key` present AND equal to `value`? This present-and-equal check by JSON
    // value equality is the deterministic test the grader builds on.
    bool matches(const std::string& key, const nlohmann::json& value) const {
        const auto entry = facts_.find(key);
        return entry != facts_.end() && entry->second == value;
    }

    std::size_t size() const noexcept { return facts_.size(); }
    bool empty() const noexcept { return facts_.empty(); }

    const std::map<std::string, nlohmann::json>& facts() const noexcept {
        return facts_;
    }

    bool operator==(const EndState&) const = default;

private:
    std::map<std::string, nlohmann::json> facts_;
};

}  // namespace zetta::agent
