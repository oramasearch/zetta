#include "zetta/util/uuid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string_view>

namespace zetta::util {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

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

std::array<std::byte, 16> uuid_to_bytes(std::string_view uuid) {
    std::array<std::byte, 16> bytes{};
    std::size_t nibble = 0;  // 0..31 across the 16 bytes
    for (const char c : uuid) {
        if (c == '-') continue;
        const int value = hex_value(c);
        if (value < 0 || nibble >= 32) return std::array<std::byte, 16>{};
        const std::size_t index = nibble / 2;
        if (nibble % 2 == 0) {
            bytes[index] = static_cast<std::byte>(value << 4);
        } else {
            bytes[index] |= static_cast<std::byte>(value);
        }
        ++nibble;
    }
    if (nibble != 32) return std::array<std::byte, 16>{};  // wrong length
    return bytes;
}

}  // namespace zetta::util