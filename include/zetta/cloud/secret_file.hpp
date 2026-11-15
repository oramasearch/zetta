// SPDX-License-Identifier: MIT

#pragma once

// A short-lived file that hands a secret to a vendor CLI without putting it on
// the CLI's command line (#770).
//
// The control planes run `aws`, `az`, `gcloud`, `oci`, `kubectl` and container
// runtimes as child processes. Anything in a child's argv is readable by every
// user on the host for as long as the child runs (`ps`, /proc/<pid>/cmdline), so
// a cluster join token must never be an argument. Each CLI can read a value from
// a file instead (`file://`, `@path`, `--metadata-from-file`, `--env-file`,
// `kubectl apply -f`); this writes the secret to a mode-0600 file inside a fresh
// mode-0700 directory and removes both when it goes out of scope.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace zetta::cloud {

class SecretFile {
public:
    // Write `contents` to a new private file. Null with `reason` set when the
    // directory or file cannot be created or written.
    static std::unique_ptr<SecretFile> create(std::string_view contents,
                                              std::string* reason = nullptr);

    ~SecretFile();
    SecretFile(const SecretFile&) = delete;
    SecretFile& operator=(const SecretFile&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    SecretFile(std::filesystem::path directory, std::filesystem::path path);

    std::filesystem::path directory_;
    std::filesystem::path path_;
};

}  // namespace zetta::cloud
