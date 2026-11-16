// SPDX-License-Identifier: MIT

#include "zetta/agent/checkpoint.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace zetta::agent {
namespace {

bool filename_only(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\") == std::string::npos &&
           name.find('\0') == std::string::npos;
}

}  // namespace

std::optional<std::string> checkpoint_directory(const std::string& selector,
                                               std::string* error) {
    const auto fail = [error](const char* reason) -> std::optional<std::string> {
        if (error != nullptr) *error = reason;
        return std::nullopt;
    };
    nlohmann::json registered = {{"base-agent", "/data/models/base-agent"},
                                 {"/data/models/base-agent", "/data/models/base-agent"}};
    if (const char* configured = std::getenv("ZETTA_AGENT_CHECKPOINTS")) {
        const auto additions = nlohmann::json::parse(configured, nullptr, false);
        if (!additions.is_object()) return fail("agent.model: invalid checkpoint registry");
        for (const auto& [name, directory] : additions.items()) {
            if (name.empty() || name.find('\0') != std::string::npos || !directory.is_string()) {
                return fail("agent.model: invalid checkpoint registry");
            }
            const auto path = directory.get<std::string>();
            if (path.find('\0') != std::string::npos ||
                !std::filesystem::path(path).is_absolute()) {
                return fail("agent.model: registry directories must be absolute paths");
            }
            registered[name] = directory;
        }
    }
    // Never interpret an unknown selector as a path, normalize it, or probe the filesystem.
    const auto selected = registered.find(selector);
    if (selected == registered.end()) return fail("agent.model: checkpoint is not registered");

    std::filesystem::path configured = selected->get<std::string>();
    configured = configured.lexically_normal();
    if (configured.filename().empty()) configured = configured.parent_path();
    std::error_code reason;
    const auto root = std::filesystem::canonical(configured, reason);
    if (reason || root != configured || !std::filesystem::is_directory(root, reason) || reason) {
        return fail("agent.model: registered checkpoint must be an existing directory "
                    "without symlinks");
    }

    // The loaders use fixed filenames plus the index's shard names. Check the complete
    // top-level file set before any loader (including a warm cache) can use the directory.
    std::filesystem::directory_iterator entry(root, reason);
    if (reason) return fail("agent.model: cannot inspect registered checkpoint");
    for (const std::filesystem::directory_iterator end; entry != end; entry.increment(reason)) {
        if (reason) return fail("agent.model: cannot inspect registered checkpoint");
        const auto status = entry->symlink_status(reason);
        if (reason || std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status))) {
            return fail("agent.model: checkpoint files must be regular files without symlinks");
        }
    }
    if (reason) return fail("agent.model: cannot inspect registered checkpoint");

    const auto index_path = root / "model.safetensors.index.json";
    const bool has_index = std::filesystem::exists(index_path, reason);
    if (reason) return fail("agent.model: cannot inspect checkpoint index");
    if (has_index) {
        std::ifstream input(index_path, std::ios::binary);
        if (!input) return fail("agent.model: cannot read checkpoint index");
        const auto index = nlohmann::json::parse(input, nullptr, false);
        if (!index.is_object() || !index.contains("weight_map") ||
            !index.at("weight_map").is_object()) {
            return fail("agent.model: invalid checkpoint index");
        }
        for (const auto& file : index.at("weight_map")) {
            if (!file.is_string() || !filename_only(file.get<std::string>())) {
                return fail("agent.model: checkpoint shard must name a file in its "
                            "registered directory");
            }
            const auto status =
                std::filesystem::symlink_status(root / file.get<std::string>(), reason);
            if (reason || !std::filesystem::is_regular_file(status)) {
                return fail("agent.model: checkpoint shard must be a regular file "
                            "without symlinks");
            }
        }
    }
    return root.string();
}

}  // namespace zetta::agent
