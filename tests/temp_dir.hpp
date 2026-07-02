#pragma once

#include <filesystem>
#include <system_error>

#include "zetta/util/uuid.hpp"

namespace zetta::test {

// RAII unique temporary directory under the system temp path, removed on
// destruction. Gives each test an isolated data_dir so persistence tests don't
// collide or leak.
class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("zetta-test-" + new_uuid())) {
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace zetta::test
