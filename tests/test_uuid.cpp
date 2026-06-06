#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <string>

#include "zetta/uuid.hpp"

using zetta::new_uuid;

TEST(Uuid, CanonicalV4Format) {
    const std::string u = new_uuid();
    ASSERT_EQ(u.size(), 36u);
    EXPECT_EQ(u[8], '-');
    EXPECT_EQ(u[13], '-');
    EXPECT_EQ(u[18], '-');
    EXPECT_EQ(u[23], '-');
    EXPECT_EQ(u[14], '4');  // version nibble
    EXPECT_NE(std::string("89ab").find(u[19]), std::string::npos);  // variant

    for (std::size_t i = 0; i < u.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const char c = u[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(hex) << "non-hex char '" << c << "' at index " << i;
    }
}

TEST(Uuid, ValuesAreUnique) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(seen.insert(new_uuid()).second) << "collision at " << i;
    }
}
