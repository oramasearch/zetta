#include "private_directory.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "zetta/util/uuid.hpp"

namespace zetta::util {
namespace {

class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0)
            ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }

private:
    int value_;
};

bool fail(const std::filesystem::path& path, std::string* reason) {
    if (reason)
        *reason = "cannot access " + path.string() + ": " + std::strerror(errno);
    return false;
}

bool valid_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos &&
           name.find('\0') == std::string_view::npos;
}

}  // namespace

PrivateDirectory::PrivateDirectory(std::filesystem::path path, int descriptor)
    : path_(std::move(path)), descriptor_(descriptor) {}

PrivateDirectory::~PrivateDirectory() { ::close(descriptor_); }

std::unique_ptr<PrivateDirectory>
PrivateDirectory::open(const std::filesystem::path& path, std::string* reason) {
    // The data directory may be new; no secrets are put in it directly.
    std::error_code error_code;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error_code);
    }
    if (error_code) {
        if (reason)
            *reason = "cannot create " + path.parent_path().string() + ": " +
                      error_code.message();
        return nullptr;
    }
    if (::mkdir(path.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
        fail(path, reason);
        return nullptr;
    }
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        fail(path, reason);
        return nullptr;
    }
    auto directory =
        std::unique_ptr<PrivateDirectory>(new PrivateDirectory(path, descriptor));
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || status.st_uid != ::geteuid()) {
        if (reason)
            *reason = "not an owned private directory: " + path.string();
        return nullptr;
    }
    // Upgrade an older directory before reading or writing any material.
    if (::fchmod(descriptor, S_IRWXU) != 0) {
        fail(path, reason);
        return nullptr;
    }
    return directory;
}

bool PrivateDirectory::read(std::string_view name, std::optional<std::string>& contents,
                            std::string* reason) const {
    contents.reset();
    if (!valid_name(name)) {
        if (reason)
            *reason = "invalid private file name";
        return false;
    }
    const std::string filename(name);
    const Descriptor file(::openat(descriptor_, filename.c_str(),
                                   O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0) {
        if (errno == ENOENT)
            return true;
        return fail(path_ / filename, reason);
    }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() || status.st_nlink != 1) {
        if (reason)
            *reason = "not an owned, single-link regular private file: " +
                      (path_ / filename).string();
        return false;
    }
    if (::fchmod(file.get(), S_IRUSR | S_IWUSR) != 0)
        return fail(path_ / filename, reason);
    std::string result;
    std::array<char, 4096> buffer;
    for (;;) {
        const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return fail(path_ / filename, reason);
        }
        if (count == 0)
            break;
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    contents = std::move(result);
    return true;
}

PrivateDirectory::PendingFile::PendingFile(const PrivateDirectory& directory,
                                           std::string name, int descriptor)
    : directory_(directory), name_(std::move(name)), descriptor_(descriptor) {}

PrivateDirectory::PendingFile::~PendingFile() {
    ::close(descriptor_);
    ::unlinkat(directory_.descriptor_, name_.c_str(), 0);
}

std::unique_ptr<PrivateDirectory::PendingFile>
PrivateDirectory::create_file(std::string* reason) const {
    const std::string temporary = ".private-" + new_uuid();
    const int descriptor =
        ::openat(descriptor_, temporary.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        fail(path_ / temporary, reason);
        return nullptr;
    }
    return std::unique_ptr<PendingFile>(new PendingFile(*this, temporary, descriptor));
}

bool PrivateDirectory::write(std::string_view name, std::string_view contents,
                             std::string* reason) const {
    if (!valid_name(name)) {
        if (reason)
            *reason = "invalid private file name";
        return false;
    }
    const std::string filename(name);
    const auto file = create_file(reason);
    if (!file)
        return false;
    // No published path points at a partially written secret. The creation
    // mode is already private, even with a process umask of zero.
    bool success = ::fchmod(file->descriptor(), S_IRUSR | S_IWUSR) == 0;
    std::size_t written = 0;
    while (success && written < contents.size()) {
        const ssize_t count = ::write(file->descriptor(), contents.data() + written,
                                      contents.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            success = false;
        } else {
            written += static_cast<std::size_t>(count);
        }
    }
    if (success)
        success = ::fsync(file->descriptor()) == 0;
    if (success)
        success = ::renameat(descriptor_, file->name_.c_str(), descriptor_,
                             filename.c_str()) == 0;
    if (!success) {
        fail(path_ / filename, reason);
    }
    return success;
}

}  // namespace zetta::util
