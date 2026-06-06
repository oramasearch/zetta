#include "zetta/uuid.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>

namespace zetta {

std::string new_uuid() {
    // 16 random bytes from the platform entropy source, with the version (4)
    // and variant (1, the "10" high bits) fields set per RFC 4122.
    std::random_device rd;
    std::array<std::uint8_t, 16> b{};
    for (std::uint8_t& byte : b) {
        byte = static_cast<std::uint8_t>(rd() & 0xFFu);
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);  // variant 1

    char buf[37];
    std::snprintf(
        buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(buf);
}

}  // namespace zetta
