#pragma once

// Internal base64 (standard alphabet, '='-padded). Shared by the cluster
// transport (SYNC document blobs + large-object bodies) and the signal plane
// (the opaque payload inside a signal envelope). Kept in zetta-core's src/core
// so both adapters reuse one implementation rather than each carrying a copy.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zetta::detail {

inline constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64_encode(std::span<const std::byte> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 3 <= data.size()) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8) |
            static_cast<std::uint32_t>(data[index + 2]);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 6) & 0x3f]);
        out.push_back(kBase64Alphabet[triple & 0x3f]);
        index += 3;
    }
    if (const std::size_t remaining = data.size() - index; remaining > 0) {
        std::uint32_t triple = static_cast<std::uint32_t>(data[index]) << 16;
        if (remaining == 2) {
            triple |= static_cast<std::uint32_t>(data[index + 1]) << 8;
        }
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(remaining == 2 ? kBase64Alphabet[(triple >> 6) & 0x3f]
                                     : '=');
        out.push_back('=');
    }
    return out;
}

inline std::vector<std::byte> base64_decode(std::string_view text) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;  // '=' padding or invalid
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3 + 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        const int value = value_of(c);
        if (value < 0) continue;  // skip padding / whitespace
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((accumulator >> bits) & 0xff));
        }
    }
    return out;
}

inline std::string base64_encode_bytes(std::string_view data) {
    return base64_encode(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

inline std::string base64_decode_to_string(std::string_view text) {
    const std::vector<std::byte> bytes = base64_decode(text);
    std::string out(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i] = static_cast<char>(bytes[i]);
    }
    return out;
}

}  // namespace zetta::detail
