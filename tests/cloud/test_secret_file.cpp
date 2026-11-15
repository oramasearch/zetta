// SPDX-License-Identifier: MIT

// SecretFile (#770): the private file a control plane hands a vendor CLI instead
// of putting the cluster join token on its argv.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "secret_argv.hpp"
#include "zetta/cloud/secret_file.hpp"

using zetta::cloud::SecretFile;

TEST(SecretFile, HoldsTheValueOwnerOnlyAndIsRemovedOnScopeExit) {
    std::filesystem::path path;
    std::filesystem::path directory;
    {
        std::string reason;
        const auto file = SecretFile::create("ZETTA_CLUSTER_TOKEN=abc\n", &reason);
        ASSERT_NE(file, nullptr) << reason;
        path = file->path();
        directory = path.parent_path();
        EXPECT_EQ(zetta::test::read_whole_file(path), "ZETTA_CLUSTER_TOKEN=abc\n");
        EXPECT_TRUE(zetta::test::owner_only(path));
        // The directory is private too, so no other user can list or swap the file.
        EXPECT_TRUE(zetta::test::owner_only(directory));
    }
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(SecretFile, EachFileIsFresh) {
    const auto first = SecretFile::create("one");
    const auto second = SecretFile::create("two");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->path(), second->path());
    EXPECT_EQ(zetta::test::read_whole_file(first->path()), "one");
    EXPECT_EQ(zetta::test::read_whole_file(second->path()), "two");
}
