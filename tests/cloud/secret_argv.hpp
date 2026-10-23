#pragma once

// Helpers for the #770 provisioner tests: a cluster join token must never be an
// argument of a vendor CLI (argv is readable by every local user), so each test
// asserts the token is absent from every argv and reads the private file the
// control plane handed the CLI instead — during the call, because the file is
// removed as soon as the command returns.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace zetta::test {

// True when any argument of any recorded call contains `needle`.
inline bool any_argv_contains(const std::vector<std::vector<std::string>>& calls,
                              std::string_view needle) {
    for (const std::vector<std::string>& argv : calls) {
        for (const std::string& argument : argv) {
            if (argument.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}

// The contents of `path`, or empty when it cannot be read.
inline std::string read_whole_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// True when `path` is readable and writable by its owner only (mode 0600).
inline bool owner_only(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    const fs::perms permissions = fs::status(path).permissions();
    return (permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none;
}

}  // namespace zetta::test
