#include <gtest/gtest.h>

#include <string>

#include "zetta/util/sha256.hpp"

using zetta::sha256_hex;

// NIST / well-known SHA-256 vectors.
TEST(Sha256, KnownVectors) {
    EXPECT_EQ(
        sha256_hex(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        sha256_hex("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(
        sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MultiBlockAndBinary) {
    // > 64 bytes forces multiple compression blocks; embedded NUL is fine.
    const std::string long_input(1000, 'a');
    EXPECT_EQ(
        sha256_hex(long_input),
        "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    std::string with_nul = "a";
    with_nul.push_back('\0');
    with_nul.push_back('b');
    EXPECT_EQ(sha256_hex(with_nul).size(), 64u);  // valid digest over binary
}
