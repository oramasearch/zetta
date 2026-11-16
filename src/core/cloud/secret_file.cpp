// SPDX-License-Identifier: MIT

#include "zetta/cloud/secret_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include "zetta/util/uuid.hpp"

namespace zetta::cloud {

namespace {

void set_reason(std::string* reason, std::string text) {
    if (reason) *reason = std::move(text);
}

}  // namespace

SecretFile::SecretFile(std::filesystem::path directory, std::filesystem::path path)
    : directory_(std::move(directory)), path_(std::move(path)) {}

std::unique_ptr<SecretFile> SecretFile::create(std::string_view contents,
                                               std::string* reason) {
    namespace fs = std::filesystem;
    std::error_code error_code;
    const fs::path base = fs::temp_directory_path(error_code);
    if (error_code) {
        set_reason(reason, "no temporary directory: " + error_code.message());
        return nullptr;
    }
    // mkdir with 0700 fails if the name exists, so another user cannot have
    // prepared the directory in advance.
    const fs::path directory = base / ("zetta-secret-" + zetta::util::new_uuid());
    if (::mkdir(directory.c_str(), S_IRWXU) != 0) {
        set_reason(reason, "cannot create " + directory.string() + ": " +
                               std::strerror(errno));
        return nullptr;
    }
    const fs::path path = directory / "value";
    const int descriptor =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
               S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        set_reason(reason, "cannot create " + path.string() + ": " + std::strerror(errno));
        fs::remove(directory, error_code);
        return nullptr;
    }
    std::size_t written = 0;
    while (written < contents.size()) {
        const ssize_t result =
            ::write(descriptor, contents.data() + written, contents.size() - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            set_reason(reason, "cannot write " + path.string() + ": " + std::strerror(errno));
            ::close(descriptor);
            fs::remove_all(directory, error_code);
            return nullptr;
        }
        written += static_cast<std::size_t>(result);
    }
    ::close(descriptor);
    return std::unique_ptr<SecretFile>(new SecretFile(directory, path));
}

SecretFile::~SecretFile() {
    std::error_code error_code;
    std::filesystem::remove_all(directory_, error_code);
}

}  // namespace zetta::cloud
