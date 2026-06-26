#include "zetta/azure_cli_control_plane.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

namespace zetta {

namespace {

// Append a flag and its value to an argv vector (kept terse — the binding builds
// several of these). No shell is involved: argv is exec'd directly.
void flag(std::vector<std::string>& argv, std::string name, std::string value) {
    argv.push_back(std::move(name));
    argv.push_back(std::move(value));
}

// The flags every `az vmss` call shares: which subscription / resource group /
// scale set the burst pool is.
void append_pool(std::vector<std::string>& argv, const AzureConfig& config) {
    flag(argv, "--subscription", config.subscription_id);
    flag(argv, "--resource-group", config.resource_group);
    flag(argv, "--name", config.pool_name);
}

// Parse an `az -o json` array of ProvisionedNode-shaped objects, skipping
// anything malformed at the validated boundary. A production binding projects
// `az vmss list-instances` output into this shape (instance → { node_id, address,
// capability }); the reference binding consumes that shape directly so its
// parsing stays aligned with ProvisionedNode::from_json.
std::vector<ProvisionedNode> parse_nodes(const std::string& json_text) {
    std::vector<ProvisionedNode> nodes;
    const nlohmann::json document =
        nlohmann::json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_array()) return nodes;
    for (const nlohmann::json& element : document) {
        if (std::optional<ProvisionedNode> node = ProvisionedNode::from_json(element)) {
            nodes.push_back(std::move(*node));
        }
    }
    return nodes;
}

}  // namespace

CommandRunner default_command_runner() {
    return [](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (argv.empty()) {
            result.exit_code = -1;
            result.stderr_text = "empty command";
            return result;
        }

        // One pipe carrying the child's merged stdout+stderr — a single reader
        // can drain it without the two-pipe deadlock, and JSON output lands on
        // stdout anyway. The combined text is returned as stdout_text.
        std::array<int, 2> pipe_ends{-1, -1};
        if (::pipe(pipe_ends.data()) != 0) {
            result.exit_code = -1;
            result.stderr_text = "pipe() failed";
            return result;
        }

        const ::pid_t child = ::fork();
        if (child < 0) {
            ::close(pipe_ends[0]);
            ::close(pipe_ends[1]);
            result.exit_code = -1;
            result.stderr_text = "fork() failed";
            return result;
        }

        if (child == 0) {
            // Child: send stdout + stderr down the write end, then exec the
            // program. argv must be a null-terminated char* array for execvp; this
            // is the one unavoidable C-API boundary (immediately exec'd).
            ::dup2(pipe_ends[1], STDOUT_FILENO);
            ::dup2(pipe_ends[1], STDERR_FILENO);
            ::close(pipe_ends[0]);
            ::close(pipe_ends[1]);
            std::vector<char*> raw;
            raw.reserve(argv.size() + 1);
            for (const std::string& argument : argv) {
                raw.push_back(const_cast<char*>(argument.c_str()));
            }
            raw.push_back(nullptr);
            ::execvp(raw[0], raw.data());
            ::_exit(127);  // exec failed (e.g. `az` not installed)
        }

        // Parent: drain the read end fully, then reap the child for its status.
        ::close(pipe_ends[1]);
        std::array<char, 4096> buffer{};
        for (;;) {
            const ::ssize_t count = ::read(pipe_ends[0], buffer.data(), buffer.size());
            if (count <= 0) break;
            result.stdout_text.append(buffer.data(), static_cast<std::size_t>(count));
        }
        ::close(pipe_ends[0]);

        int status = 0;
        ::waitpid(child, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return result;
    };
}

AzCliControlPlane::AzCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult AzCliControlPlane::bring_up(const AzureConfig& config,
                                            const ProvisionRequest& request) {
    ProvisionResult result;

    // Scale the burst pool to the requested count — create it if absent. The
    // stable pool name makes this idempotent (a second burst scales the existing
    // scale set rather than spawning a duplicate).
    std::vector<std::string> create{"az", "vmss", "create"};
    append_pool(create, config);
    flag(create, "--location", config.location);
    flag(create, "--instance-count", std::to_string(request.count));
    if (!config.image.empty()) flag(create, "--image", config.image);
    if (!config.vm_size.empty()) flag(create, "--vm-sku", config.vm_size);
    if (config.spot) {
        flag(create, "--priority", "Spot");
        flag(create, "--eviction-policy", "Delete");
        flag(create, "--max-price", std::to_string(config.max_price));
    }
    if (!config.managed_identity.empty()) {
        flag(create, "--assign-identity", config.managed_identity);
    }
    // Hand each node the cluster seed address at boot so it auto-joins. (A real
    // deployment supplies a full cloud-init; this illustrative custom-data carries
    // the seed peer the bootstrap reads.)
    if (!config.seed_peer.empty()) {
        flag(create, "--custom-data", "ZETTA_SEED_PEER=" + config.seed_peer);
    }

    const CommandResult created = runner_(create);
    if (created.exit_code != 0) {
        result.satisfied = false;
        result.detail = "az vmss create failed: " +
                        (created.stderr_text.empty() ? created.stdout_text
                                                     : created.stderr_text);
        return result;
    }

    // The pool is scaling; list what is up and report best-effort against the
    // requested count.
    result.nodes = list(config);
    result.satisfied = result.nodes.size() >= request.count;
    if (result.satisfied) {
        result.detail = "burst pool '" + config.pool_name + "' scaled to " +
                        std::to_string(request.count) + " node(s)";
    } else {
        result.detail = "best-effort: " + std::to_string(result.nodes.size()) +
                        " of " + std::to_string(request.count) +
                        " requested nodes up in pool '" + config.pool_name + "'";
    }
    return result;
}

bool AzCliControlPlane::destroy(const AzureConfig& config,
                                const std::string& node_id) {
    std::vector<std::string> remove{"az", "vmss", "delete-instances"};
    append_pool(remove, config);
    flag(remove, "--instance-ids", node_id);
    return runner_(remove).exit_code == 0;
}

std::vector<ProvisionedNode> AzCliControlPlane::list(
    const AzureConfig& config) const {
    std::vector<std::string> query{"az", "vmss", "list-instances"};
    append_pool(query, config);
    flag(query, "--output", "json");

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta
