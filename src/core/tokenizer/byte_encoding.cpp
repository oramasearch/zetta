#include "zetta/tokenizer/byte_encoding.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace zetta::tokenizer {

std::vector<char32_t> utf8_decode(std::string_view text) {
    std::vector<char32_t> code_points;
    code_points.reserve(text.size());
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const auto lead = static_cast<unsigned char>(text[i]);
        char32_t code_point = 0;
        int extra = 0;
        if (lead < 0x80) {
            code_point = lead;
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0F;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07;
            extra = 3;
        } else {
            code_points.push_back(0xFFFD);  // invalid lead byte
            ++i;
            continue;
        }
        if (i + extra >= n) {
            code_points.push_back(0xFFFD);  // truncated sequence
            break;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            code_point = (code_point << 6) | (cont & 0x3F);
        }
        if (!ok) {
            code_points.push_back(0xFFFD);
            ++i;  // resynchronize one byte at a time
            continue;
        }
        code_points.push_back(code_point);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return code_points;
}

void utf8_encode(char32_t code_point, std::string& out) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

namespace {

// GPT-2 bytes_to_unicode: bytes that are already printable (!..~, ¡..¬, ®..ÿ) map
// to themselves; every other byte maps to U+0100, U+0101, … in byte order.
std::array<char32_t, 256> build_byte_to_unicode() {
    const auto printable = [](int byte) {
        return (byte >= '!' && byte <= '~') || (byte >= 0xA1 && byte <= 0xAC) ||
               (byte >= 0xAE && byte <= 0xFF);
    };
    std::array<char32_t, 256> map{};
    int next = 0;
    for (int byte = 0; byte < 256; ++byte) {
        if (printable(byte)) {
            map[byte] = static_cast<char32_t>(byte);
        } else {
            map[byte] = static_cast<char32_t>(256 + next);
            ++next;
        }
    }
    return map;
}

}  // namespace

const std::array<char32_t, 256>& byte_to_unicode() {
    static const std::array<char32_t, 256> map = build_byte_to_unicode();
    return map;
}

int unicode_to_byte(char32_t code_point) {
    static const std::unordered_map<char32_t, int> inverse = [] {
        std::unordered_map<char32_t, int> table;
        const std::array<char32_t, 256>& forward = byte_to_unicode();
        for (int byte = 0; byte < 256; ++byte) {
            table[forward[byte]] = byte;
        }
        return table;
    }();
    const auto found = inverse.find(code_point);
    return found == inverse.end() ? -1 : found->second;
}

}  // namespace zetta::tokenizer
