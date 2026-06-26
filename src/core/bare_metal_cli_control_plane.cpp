#include "zetta/bare_metal_cli_control_plane.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/command_runner.hpp"
#include "zetta/uuid.hpp"

namespace zetta {

namespace {

// Append a flag and its value to an argv vector (kept terse — the binding builds
// several of these). No shell is involved: argv is exec'd directly.
void flag(std::vector<std::string>& argv, std::string name, std::string value) {
    argv.push_back(std::move(name));
    argv.push_back(std::move(value));
}

// Trim ASCII whitespace from both ends of a line.
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Try to read one ProvisionedNode-shaped JSON object out of `text` into `nodes`,
// skipping it if malformed at the validated boundary.
void push_if_node(const std::string& text, std::vector<ProvisionedNode>& nodes) {
    const nlohmann::json element =
        nlohmann::json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (element.is_discarded()) return;
    if (std::optional<ProvisionedNode> node = ProvisionedNode::from_json(element)) {
        nodes.push_back(std::move(*node));
    }
}

// Parse a container-runtime listing of ProvisionedNode-shaped objects, skipping
// anything malformed. `docker ps --format '{{json .}}'` emits **newline-delimited
// JSON** (one object per line), not an array — so parse line-by-line; also accept a
// JSON array (or a { "data"/"items": [ … ] } envelope) for robustness across
// runtimes. A production binding projects `ps` output into the { node_id, address,
// capability } shape; the reference binding consumes that shape directly so its
// parsing stays aligned with ProvisionedNode::from_json.
std::vector<ProvisionedNode> parse_nodes(const std::string& json_text) {
    std::vector<ProvisionedNode> nodes;

    const nlohmann::json whole =
        nlohmann::json::parse(json_text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (!whole.is_discarded()) {
        nlohmann::json array = whole;
        if (whole.is_object() && whole.contains("data")) array = whole["data"];
        else if (whole.is_object() && whole.contains("items")) array = whole["items"];
        if (array.is_array()) {
            for (const nlohmann::json& element : array) {
                if (std::optional<ProvisionedNode> node =
                        ProvisionedNode::from_json(element)) {
                    nodes.push_back(std::move(*node));
                }
            }
            return nodes;
        }
    }

    // Newline-delimited JSON: one object per line.
    std::size_t pos = 0;
    while (pos < json_text.size()) {
        const std::size_t newline = json_text.find('\n', pos);
        const std::size_t end =
            newline == std::string::npos ? json_text.size() : newline;
        const std::string line = trim(json_text.substr(pos, end - pos));
        if (!line.empty()) push_if_node(line, nodes);
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    return nodes;
}

// Build the `<runtime> run -d` argv that launches one detached worker container. The
// per-worker shape — label (pool key), unique name, network, GPUs, cluster seed env
// — lands here. `run -d` returns immediately (printing the container id), which is
// what makes a long-lived worker launchable through the synchronous CommandRunner.
std::vector<std::string> run_argv(const BareMetalConfig& config) {
    std::vector<std::string> run{config.runtime, "run", "-d"};
    flag(run, "--label", config.pool_label);
    flag(run, "--name", config.name_prefix + "-" + new_uuid());
    if (!config.network.empty()) flag(run, "--network", config.network);
    if (config.gpus) flag(run, "--gpus", "all");
    if (!config.seed_peer.empty()) {
        flag(run, "--env", "ZETTA_SEED_PEER=" + config.seed_peer);
    }
    run.push_back(config.image);  // the image is the trailing positional arg
    return run;
}

}  // namespace

BareMetalCliControlPlane::BareMetalCliControlPlane(CommandRunner runner)
    : runner_(runner ? std::move(runner) : default_command_runner()) {}

ProvisionResult BareMetalCliControlPlane::bring_up(const BareMetalConfig& config,
                                                   const ProvisionRequest& request) {
    ProvisionResult result;

    // Launch only the shortfall — what is already up under the pool label counts —
    // so a re-burst to the same count launches nothing (idempotent on the label).
    const std::vector<ProvisionedNode> already_up = list(config);
    std::string run_failure;
    for (std::size_t index = already_up.size(); index < request.count; ++index) {
        const CommandResult launched = runner_(run_argv(config));
        if (launched.exit_code != 0) {
            run_failure = launched.stderr_text.empty() ? launched.stdout_text
                                                       : launched.stderr_text;
            break;  // best-effort: stop at the first failure
        }
    }

    // List what is up now and report best-effort against the requested count.
    result.nodes = list(config);
    result.satisfied = result.nodes.size() >= request.count;
    if (result.satisfied) {
        result.detail = "burst pool '" + config.pool_label + "' has " +
                        std::to_string(result.nodes.size()) + " worker(s)";
    } else {
        result.detail = "best-effort: " + std::to_string(result.nodes.size()) +
                        " of " + std::to_string(request.count) +
                        " requested workers up in pool '" + config.pool_label + "'";
        if (!run_failure.empty()) {
            result.detail += " (" + config.runtime + " run failed: " + run_failure + ")";
        }
    }
    return result;
}

bool BareMetalCliControlPlane::destroy(const BareMetalConfig& config,
                                       const std::string& node_id) {
    std::vector<std::string> remove{config.runtime, "rm", "-f", node_id};
    return runner_(remove).exit_code == 0;
}

std::vector<ProvisionedNode> BareMetalCliControlPlane::list(
    const BareMetalConfig& config) const {
    std::vector<std::string> query{config.runtime, "ps"};
    flag(query, "--filter", "label=" + config.pool_label);
    flag(query, "--format", "{{json .}}");

    const CommandResult listed = runner_(query);
    if (listed.exit_code != 0) return {};
    return parse_nodes(listed.stdout_text);
}

}  // namespace zetta
