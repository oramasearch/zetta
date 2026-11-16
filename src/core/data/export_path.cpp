// SPDX-License-Identifier: MIT

#include "zetta/data/export_path.hpp"

#include <cerrno>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zetta/data/import_path.hpp"
#include "zetta/node.hpp"

#include "unique_descriptor.hpp"

// The export root (#1397, #1399): see include/zetta/data/export_path.hpp for the
// rules. Every step opens relative to the descriptor of the step before it, with
// O_NOFOLLOW, so no step re-resolves a path string a link could redirect, and the
// file is created with O_EXCL so nothing that already exists is ever written.

namespace zetta::data {
namespace {

namespace fs = std::filesystem;
using detail::UniqueDescriptor;

constexpr std::string_view kWhere = "the node's export directory (exports/ under its data directory)";

ExportResult refuse(std::string reason) {
    ExportResult result;
    result.error = std::move(reason);
    return result;
}

std::string quoted(std::string_view requested) {
    return "'" + std::string(requested) + "'";
}

std::string message_of(int error) {
    return std::error_code(error, std::generic_category()).message();
}

// True when `name` in `directory` is a symbolic link (asked without following it).
bool is_link(int directory, const std::string& name) {
    struct stat entry {};
    return ::fstatat(directory, name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISLNK(entry.st_mode);
}

// Open (creating it when missing, mode 0700) the directory `name` inside `parent`,
// refusing a symbolic link. Returns an invalid descriptor and sets `error` on failure.
UniqueDescriptor open_directory(int parent, const std::string& name, std::string& error) {
    if (::mkdirat(parent, name.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "cannot create '" + name + "': " + message_of(errno);
        return UniqueDescriptor();
    }
    UniqueDescriptor directory(
        ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory.valid()) {
        const int failure = errno;
        // macOS answers O_DIRECTORY | O_NOFOLLOW on a link with ENOTDIR, Linux with
        // ELOOP: ask the entry itself which it is.
        if (is_link(parent, name) || failure == ELOOP) {
            error = "'" + name + "' is a symbolic link; links are not followed under " +
                    std::string(kWhere);
        } else if (failure == ENOTDIR) {
            error = "'" + name + "' is not a directory";
        } else {
            error = "cannot open '" + name + "': " + message_of(failure);
        }
    }
    return directory;
}

bool write_all(int descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

}  // namespace

fs::path export_root(const Node& node) {
    return fs::path(node.config().data_dir) / "exports";
}

ExportResult write_export_file(const fs::path& root, std::string_view requested,
                               std::string_view bytes) {
    // --- lexical checks, shared with the import root: nothing below touches the
    // filesystem until these pass.
    std::string lexical_error;
    const std::vector<std::string> components =
        caller_path_components(requested, kWhere, lexical_error);
    if (components.empty()) return refuse(std::move(lexical_error));

    // --- the root: its parent (the data directory) may sit behind a link (a mounted
    // volume, macOS's /var); `exports` itself is created by the node and may not.
    const fs::path parent_path = root.has_parent_path() ? root.parent_path() : fs::path(".");
    UniqueDescriptor parent(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!parent.valid()) {
        return refuse(quoted(requested) + ": the node's data directory cannot be opened: " +
                      message_of(errno));
    }
    std::string error;
    UniqueDescriptor directory = open_directory(parent.get(), root.filename().string(), error);
    if (!directory.valid()) return refuse(quoted(requested) + ": " + error);

    // --- each directory on the way, created when missing, never a link.
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        UniqueDescriptor next = open_directory(directory.get(), components[index], error);
        if (!next.valid()) return refuse(quoted(requested) + ": " + error);
        directory = std::move(next);
    }

    // --- the file: created new, never an existing file, never through a link.
    const std::string& name = components.back();
    UniqueDescriptor file(::openat(directory.get(), name.c_str(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!file.valid()) {
        const int failure = errno;
        // O_CREAT | O_EXCL refuses a link (even a dangling one) with EEXIST, so ask
        // the entry itself which it is.
        if (failure == ELOOP || is_link(directory.get(), name)) {
            return refuse(quoted(requested) + ": '" + name +
                          "' is a symbolic link; links are not followed under " +
                          std::string(kWhere));
        }
        if (failure == EEXIST) {
            return refuse(quoted(requested) + ": already exists under " + std::string(kWhere) +
                          "; an export never overwrites a file, choose a new name");
        }
        return refuse(quoted(requested) + ": cannot create '" + name + "': " + message_of(failure));
    }
    if (!write_all(file.get(), bytes) || ::fsync(file.get()) != 0) {
        const int failure = errno;
        ::unlinkat(directory.get(), name.c_str(), 0);
        return refuse(quoted(requested) + ": the write failed: " + message_of(failure));
    }

    ExportResult result;
    for (const std::string& component : components) {
        if (!result.relative.empty()) result.relative += '/';
        result.relative += component;
    }
    result.path = root / result.relative;
    return result;
}

ExportResult write_export_file(const Node& node, std::string_view requested,
                               std::string_view bytes) {
    return write_export_file(export_root(node), requested, bytes);
}

}  // namespace zetta::data
