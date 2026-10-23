#pragma once

#include <cstdlib>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace zetta::test {

// A test's operator configuration; restore it after the serving threads have stopped (#1411).
class CheckpointRegistration {
public:
    CheckpointRegistration() {
        if (const char* previous = std::getenv("ZETTA_AGENT_CHECKPOINTS")) previous_ = previous;
    }
    ~CheckpointRegistration() {
        if (previous_) {
            setenv("ZETTA_AGENT_CHECKPOINTS", previous_->c_str(), 1);
        } else {
            unsetenv("ZETTA_AGENT_CHECKPOINTS");
        }
    }
    void configure(const std::string& value) { setenv("ZETTA_AGENT_CHECKPOINTS", value.c_str(), 1); }
    void add(const std::string& selector, const std::string& directory) {
        nlohmann::json registry = nlohmann::json::object();
        if (const char* configured = std::getenv("ZETTA_AGENT_CHECKPOINTS")) {
            registry = nlohmann::json::parse(configured);
        }
        registry[selector] = directory;
        configure(registry.dump());
    }

private:
    std::optional<std::string> previous_;
};

}  // namespace zetta::test
