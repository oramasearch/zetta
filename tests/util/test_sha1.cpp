// The native SHA-1 behind MySQL's mysql_native_password (#895), checked against
// the FIPS 180-4 / RFC 3174 test vectors.
#include <gtest/gtest.h>

#include <string>

#include "zetta/util/sha1.hpp"

namespace {

std::string hex(const std::string& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const char byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        out.push_back(kDigits[value >> 4]);
        out.push_back(kDigits[value & 0x0f]);
    }
    return out;
}

}  // namespace

TEST(Sha1, MatchesTheStandardVectors) {
    EXPECT_EQ(hex(zetta::util::sha1_digest("")),
              "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(hex(zetta::util::sha1_digest("abc")),
              "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes: the length no longer fits the first padded block.
    EXPECT_EQ(hex(zetta::util::sha1_digest(
                  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(hex(zetta::util::sha1_digest(std::string(1000000, 'a'))),
              "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(Sha1, DigestIsTwentyRawBytes) {
    EXPECT_EQ(zetta::util::sha1_digest("anything").size(), 20u);
}
