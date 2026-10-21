#include "zetta/util/sha1.hpp"

#include <array>
#include <bit>
#include <cstdint>

// A native SHA-1 (FIPS 180-4), kept for the one protocol that still needs it —
// see sha1.hpp. Mirrors the shape of sha256.cpp.
namespace zetta::util {
namespace {

void compress(std::array<std::uint32_t, 5>& state, const unsigned char* block) {
    std::array<std::uint32_t, 80> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                       (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < 80; ++index) {
        words[index] = std::rotl(words[index - 3] ^ words[index - 8] ^
                                     words[index - 14] ^ words[index - 16],
                                 1);
    }

    std::uint32_t first = state[0];
    std::uint32_t second = state[1];
    std::uint32_t third = state[2];
    std::uint32_t fourth = state[3];
    std::uint32_t fifth = state[4];
    for (std::size_t index = 0; index < 80; ++index) {
        std::uint32_t mixed = 0;
        std::uint32_t constant = 0;
        if (index < 20) {
            mixed = (second & third) | (~second & fourth);
            constant = 0x5a827999u;
        } else if (index < 40) {
            mixed = second ^ third ^ fourth;
            constant = 0x6ed9eba1u;
        } else if (index < 60) {
            mixed = (second & third) | (second & fourth) | (third & fourth);
            constant = 0x8f1bbcdcu;
        } else {
            mixed = second ^ third ^ fourth;
            constant = 0xca62c1d6u;
        }
        const std::uint32_t next =
            std::rotl(first, 5) + mixed + fifth + constant + words[index];
        fifth = fourth;
        fourth = third;
        third = std::rotl(second, 30);
        second = first;
        first = next;
    }
    state[0] += first;
    state[1] += second;
    state[2] += third;
    state[3] += fourth;
    state[4] += fifth;
}

}  // namespace

std::string sha1_digest(std::string_view bytes) {
    std::array<std::uint32_t, 5> state = {0x67452301u, 0xefcdab89u, 0x98badcfeu,
                                          0x10325476u, 0xc3d2e1f0u};

    // Whole blocks straight from the input, then the padded tail.
    std::size_t offset = 0;
    for (; offset + 64 <= bytes.size(); offset += 64) {
        compress(state, reinterpret_cast<const unsigned char*>(bytes.data() + offset));
    }
    std::array<unsigned char, 128> tail{};
    const std::size_t remaining = bytes.size() - offset;
    for (std::size_t index = 0; index < remaining; ++index) {
        tail[index] = static_cast<unsigned char>(bytes[offset + index]);
    }
    tail[remaining] = 0x80;
    const std::size_t tail_length = remaining + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (std::size_t index = 0; index < 8; ++index) {
        tail[tail_length - 1 - index] =
            static_cast<unsigned char>((bit_length >> (index * 8)) & 0xff);
    }
    compress(state, tail.data());
    if (tail_length == 128) compress(state, tail.data() + 64);

    std::string digest(20, '\0');
    for (std::size_t index = 0; index < 5; ++index) {
        digest[index * 4] = static_cast<char>((state[index] >> 24) & 0xff);
        digest[index * 4 + 1] = static_cast<char>((state[index] >> 16) & 0xff);
        digest[index * 4 + 2] = static_cast<char>((state[index] >> 8) & 0xff);
        digest[index * 4 + 3] = static_cast<char>(state[index] & 0xff);
    }
    return digest;
}

}  // namespace zetta::util
