#include "zetta/cloud/node_bootstrap_env.hpp"

#include <cstdlib>
#include <utility>

namespace zetta::cloud {

const char* process_env(const char* name) { return std::getenv(name); }

std::vector<std::string> split_seed_peers(std::string_view value) {
    const auto is_separator = [](char character) {
        return character == ',' || character == ' ' || character == '\t' ||
               character == '\n' || character == '\r';
    };
    std::vector<std::string> seeds;
    std::size_t index = 0;
    while (index < value.size()) {
        while (index < value.size() && is_separator(value[index])) ++index;
        const std::size_t begin = index;
        while (index < value.size() && !is_separator(value[index])) ++index;
        if (index > begin) {
            seeds.emplace_back(value.substr(begin, index - begin));
        }
    }
    return seeds;
}

namespace {

// The value of `name` through `read`, or an empty string when unset — so a
// caller can treat unset and blank identically.
std::string env_value(const EnvReader& read, const char* name) {
    const char* value = read(name);
    return value ? std::string(value) : std::string();
}

}  // namespace

BootstrapEnvApplied apply_bootstrap_env(NodeBootstrap& config,
                                        const EnvReader& read) {
    BootstrapEnvApplied applied;

    // ZETTA_SEED_PEER -> --peer. Explicit --peer wins: env fills only an empty
    // seed list. A list value (comma/space separated) mirrors repeated --peer.
    if (!config.peer_from_cli) {
        for (std::string& seed :
             split_seed_peers(env_value(read, "ZETTA_SEED_PEER"))) {
            applied.seeds.push_back(seed);
            config.seeds.push_back(std::move(seed));
        }
    }

    // ZETTA_CLUSTER_TOKEN -> --join-token. Skipped when the operator set a token
    // explicitly, and when running plaintext — env never forces security on.
    // The token is passed through unvalidated, exactly as --join-token is; a
    // malformed token fails loudly in load_or_create_cluster_security.
    if (!config.join_token_from_cli && !config.cluster_plaintext) {
        std::string token = env_value(read, "ZETTA_CLUSTER_TOKEN");
        if (!token.empty()) {
            config.join_token = std::move(token);
            applied.join_token = true;
        }
    }

    // ZETTA_DATA_DIR -> --data-dir. Explicit --data-dir wins.
    if (!config.data_dir_from_cli) {
        std::string data_dir = env_value(read, "ZETTA_DATA_DIR");
        if (!data_dir.empty()) {
            config.data_dir = data_dir;
            applied.data_dir = std::move(data_dir);
        }
    }

    return applied;
}

}  // namespace zetta::cloud
