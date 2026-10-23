#include "zetta/data/import_path.hpp"

#include <cerrno>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zetta/node.hpp"

// The import root (#1391, #1433): see include/zetta/data/import_path.hpp for the
// rules. The walk opens each component relative to the descriptor of the one before
// it with O_NOFOLLOW, so a symbolic link anywhere on the way is refused by the kernel
// instead of being followed, and no step depends on a path string being re-resolved.

namespace zetta::data {
namespace {

namespace fs = std::filesystem;

// Owns one file descriptor; closes it when replaced or destroyed.
class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    ~Descriptor() { reset(); }

    int get() const { return value_; }
    bool valid() const { return value_ >= 0; }

private:
    void reset() {
        if (value_ >= 0) ::close(value_);
        value_ = -1;
    }
    int value_;
};

ImportPathResult refuse(std::string reason) {
    ImportPathResult result;
    result.error = std::move(reason);
    return result;
}

std::string quoted(std::string_view requested) {
    return "'" + std::string(requested) + "'";
}

constexpr std::string_view kWhere = "the node's import directory (imports/ under its data directory)";

}  // namespace

fs::path import_root(const Node& node) {
    return fs::path(node.config().data_dir) / "imports";
}

ImportPathResult resolve_import_path(const fs::path& root, std::string_view requested,
                                     ImportEntry expected) {
    // --- lexical checks: nothing below touches the filesystem until these pass, so a
    // path outside the root is refused the same way whether or not it exists.
    if (requested.empty()) return refuse("a path is required, relative to " + std::string(kWhere));
    if (requested.find('\0') != std::string_view::npos) {
        return refuse("the path contains a NUL byte");
    }
    if (requested.find('\\') != std::string_view::npos) {
        return refuse(quoted(requested) + ": a backslash is not allowed in a path under " +
                      std::string(kWhere));
    }
    if (requested.starts_with("~")) {
        return refuse(quoted(requested) + ": '~' is not expanded; name the path relative to " +
                      std::string(kWhere));
    }
    const fs::path given(requested);
    if (given.is_absolute() || given.has_root_directory() || given.has_root_name()) {
        return refuse(quoted(requested) + ": an absolute path is not allowed; name it relative to " +
                      std::string(kWhere));
    }
    std::vector<std::string> components;
    for (const fs::path& component : given) {
        const std::string name = component.string();
        if (name == "..") {
            return refuse(quoted(requested) + ": '..' is not allowed; the path must stay under " +
                          std::string(kWhere));
        }
        if (name.empty() || name == ".") continue;
        components.push_back(name);
    }
    if (components.empty()) {
        return refuse(quoted(requested) + ": names the import directory itself, not an entry in it");
    }

    // --- the walk. The root itself may sit behind a link (a mounted volume, macOS's
    // /var -> /private/var); everything below it may not.
    Descriptor directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory.valid()) {
        return refuse(quoted(requested) + ": nothing is staged on this node; " +
                      std::string(kWhere) + " does not exist");
    }
    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool last = index + 1 == components.size();
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
        if (!last || expected == ImportEntry::Directory) {
            flags |= O_DIRECTORY;
        } else {
            flags |= O_NONBLOCK;  // a FIFO must not block the open; its type is refused below
        }
        Descriptor next(::openat(directory.get(), components[index].c_str(), flags));
        if (!next.valid()) {
            const int error = errno;
            // macOS answers O_DIRECTORY | O_NOFOLLOW on a link with ENOTDIR, Linux
            // with ELOOP: ask the entry itself (without following it) which it is.
            struct stat entry {};
            const bool is_link =
                ::fstatat(directory.get(), components[index].c_str(), &entry,
                          AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISLNK(entry.st_mode);
            if (is_link || error == ELOOP || error == EMLINK) {
                return refuse(quoted(requested) + ": '" + components[index] +
                              "' is a symbolic link; links are not followed under " +
                              std::string(kWhere));
            }
            if (error == ENOTDIR) {
                return refuse(quoted(requested) + ": '" + components[index] +
                              "' is not a directory");
            }
            if (error == ENOENT) {
                return refuse(quoted(requested) + ": no such " +
                              (last && expected == ImportEntry::File ? "file" : "directory") +
                              " under " + std::string(kWhere));
            }
            return refuse(quoted(requested) + ": cannot open '" + components[index] +
                          "': " + std::error_code(error, std::generic_category()).message());
        }
        if (last) {
            struct stat status {};
            if (::fstat(next.get(), &status) != 0) {
                return refuse(quoted(requested) + ": cannot read what it names");
            }
            if (expected == ImportEntry::File && !S_ISREG(status.st_mode)) {
                return refuse(quoted(requested) + ": not a regular file");
            }
        }
        directory = std::move(next);
    }

    // --- the real path of what was found must still be under the real path of the
    // root (a belt to the O_NOFOLLOW braces: a rename racing the walk shows up here).
    std::error_code error;
    const fs::path real_root = fs::canonical(root, error);
    if (error) return refuse(quoted(requested) + ": " + std::string(kWhere) + " cannot be read");
    fs::path joined = real_root;
    for (const std::string& component : components) joined /= component;
    const fs::path real = fs::canonical(joined, error);
    if (error) return refuse(quoted(requested) + ": no longer present under " + std::string(kWhere));
    const fs::path relative = real.lexically_relative(real_root);
    if (relative.empty() || *relative.begin() == "..") {
        return refuse(quoted(requested) + ": resolves outside " + std::string(kWhere));
    }

    ImportPathResult result;
    result.path = real;
    return result;
}

ImportPathResult resolve_import_path(const Node& node, std::string_view requested,
                                     ImportEntry expected) {
    return resolve_import_path(import_root(node), requested, expected);
}

}  // namespace zetta::data
