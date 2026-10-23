#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iterator>

#include "../../src/core/util/private_directory.hpp"
#include "../temp_dir.hpp"

namespace {

class UmaskGuard {
public:
    explicit UmaskGuard(mode_t mask) : previous_(::umask(mask)) {}
    ~UmaskGuard() { ::umask(previous_); }

private:
    mode_t previous_;
};

}  // namespace

TEST(PrivateDirectory, OwnerOnlyAtCreationBeforeAnyBytesAreWritten) {
    zetta::test::TempDir temporary;
    const UmaskGuard umask(0);
    const auto path = temporary.path() / "new" / "security";
    std::string reason;
    const auto directory = zetta::util::PrivateDirectory::open(path, &reason);
    ASSERT_TRUE(directory) << reason;
    struct stat status{};
    ASSERT_EQ(::stat(path.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0700);
    const auto file = directory->create_file(&reason);
    ASSERT_TRUE(file) << reason;
    ASSERT_EQ(::fstat(file->descriptor(), &status), 0);
    EXPECT_EQ(status.st_size, 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    EXPECT_EQ(::fcntl(file->descriptor(), F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
}

TEST(PrivateDirectory, FailedPublishRemovesThePrivateStagingFile) {
    zetta::test::TempDir temporary;
    const auto path = temporary.path() / "security";
    std::string reason;
    const auto directory = zetta::util::PrivateDirectory::open(path, &reason);
    ASSERT_TRUE(directory) << reason;
    std::filesystem::create_directory(path / "occupied");
    EXPECT_FALSE(directory->write("occupied", "dummy", &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(path),
                            std::filesystem::directory_iterator()),
              1);
    for (const auto& name : {"../escape", "/absolute", "..", ".", ""}) {
        EXPECT_FALSE(directory->write(name, "dummy", &reason));
    }
}

TEST(PrivateDirectory, AReplacedDirectoryPathCannotRedirectSecretWrites) {
    zetta::test::TempDir temporary;
    const auto path = temporary.path() / "security";
    const auto moved = temporary.path() / "original";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directory(outside);
    std::string reason;
    const auto directory = zetta::util::PrivateDirectory::open(path, &reason);
    ASSERT_TRUE(directory) << reason;
    std::filesystem::rename(path, moved);
    std::filesystem::create_directory_symlink(outside, path);
    ASSERT_TRUE(directory->write("value", "dummy", &reason)) << reason;
    EXPECT_TRUE(std::filesystem::is_empty(outside));
    EXPECT_TRUE(std::filesystem::is_regular_file(moved / "value"));
    std::optional<std::string> contents;
    ASSERT_TRUE(directory->read("value", contents, &reason)) << reason;
    EXPECT_EQ(contents, "dummy");
}
