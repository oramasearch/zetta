#include "zetta/cloud/node_bootstrap_env.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "zetta/cluster/security.hpp"

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

// `text` without leading or trailing whitespace (a token file usually ends in a
// newline; a pasted variable may carry stray spaces).
std::string trimmed(std::string_view text) {
    const auto is_space = [](char character) {
        return character == ' ' || character == '\t' || character == '\n' ||
               character == '\r' || character == '\f' || character == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

// A join token is 64 characters; anything past this is not a token file.
constexpr std::size_t kMaxTokenFileBytes = 4096;

// Read the token file at `path` into `token` (trimmed). False with `error`
// naming the file when it cannot be read, is too large, or holds nothing.
// Appends a warning when users other than the owner and group may read it.
bool read_token_file(const std::string& path, std::string& token,
                     std::string& error, std::vector<std::string>& warnings) {
    namespace fs = std::filesystem;
    const std::string source = "ZETTA_CLUSTER_TOKEN_FILE (" + path + ")";
    std::error_code error_code;
    const fs::file_status status = fs::status(path, error_code);
    if (error_code || !fs::is_regular_file(status)) {
        error = source + " is not a readable file";
        return false;
    }
    const std::uintmax_t size = fs::file_size(path, error_code);
    if (error_code || size > kMaxTokenFileBytes) {
        error = source + " is not a token file (larger than " +
                std::to_string(kMaxTokenFileBytes) + " bytes)";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = source + " cannot be opened";
        return false;
    }
    const std::string contents{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
    token = trimmed(contents);
    if (token.empty()) {
        error = source + " is empty";
        return false;
    }
    if ((status.permissions() & fs::perms::others_read) != fs::perms::none) {
        warnings.push_back(source +
                           " is readable by every user on this host; restrict it "
                           "to the node's user (mode 0400 or 0440)");
    }
    return true;
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

    // ZETTA_CLUSTER_TOKEN_FILE / ZETTA_CLUSTER_TOKEN -> --join-token. Skipped
    // when the operator set a token explicitly on the CLI. The file is preferred
    // (#770): its value never sits in the process environment. A file and a
    // variable that disagree refuse the start rather than pick one silently. The
    // token is trimmed and validated here so a bad one is reported against its
    // source, not as a --join-token the operator never typed. Inter-node mTLS is
    // always on (#799).
    if (!config.join_token_from_cli) {
        const std::string token_file = trimmed(env_value(read, "ZETTA_CLUSTER_TOKEN_FILE"));
        const std::string env_token = trimmed(env_value(read, "ZETTA_CLUSTER_TOKEN"));
        std::string token;
        std::string source;
        if (!token_file.empty()) {
            source = "ZETTA_CLUSTER_TOKEN_FILE (" + token_file + ")";
            if (!read_token_file(token_file, token, applied.error, applied.warnings)) {
                return applied;
            }
            if (!env_token.empty() && env_token != token) {
                applied.error =
                    "ZETTA_CLUSTER_TOKEN and " + source +
                    " hold different join tokens; set only one of them";
                return applied;
            }
        } else if (!env_token.empty()) {
            source = "ZETTA_CLUSTER_TOKEN";
            token = env_token;
        }
        if (!token.empty()) {
            if (!zetta::cluster::valid_join_token(token)) {
                applied.error = "the join token from " + source +
                                " must be 64 lowercase hex characters";
                return applied;
            }
            config.join_token = std::move(token);
            applied.join_token = true;
            applied.join_token_source = std::move(source);
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

void scrub_bootstrap_secrets() { ::unsetenv("ZETTA_CLUSTER_TOKEN"); }

}  // namespace zetta::cloud
