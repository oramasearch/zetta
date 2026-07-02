#include "zetta/util/sha256.hpp"

#include <array>
#include <cstdint>

// A small, self-contained SHA-256 (FIPS 180-4) so zetta-core has no crypto
// dependency. Not constant-time; we only use it for content fingerprints, never
// secrets.
namespace zetta {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

}  // namespace

std::string sha256_hex(std::string_view bytes) {
    std::array<std::uint32_t, 8> hash = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                         0xa54ff53a, 0x510e527f, 0x9b05688c,
                                         0x1f83d9ab, 0x5be0cd19};

    // Pad: append 0x80, then zeros, then the 64-bit big-endian bit length, so
    // the total is a multiple of 64 bytes.
    std::string message(bytes);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) message.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<char>((bit_length >> shift) & 0xff));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::array<std::uint32_t, 64> words = {};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] =
                (static_cast<std::uint32_t>(
                     static_cast<unsigned char>(message[offset + i * 4])) << 24) |
                (static_cast<std::uint32_t>(
                     static_cast<unsigned char>(message[offset + i * 4 + 1])) << 16) |
                (static_cast<std::uint32_t>(
                     static_cast<unsigned char>(message[offset + i * 4 + 2])) << 8) |
                static_cast<std::uint32_t>(
                    static_cast<unsigned char>(message[offset + i * 4 + 3]));
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const std::uint32_t s1 =
                rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        std::uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
        std::uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + words[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    }

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const std::uint32_t word : hash) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(kHexDigits[(word >> shift) & 0xf]);
        }
    }
    return hex;
}

}  // namespace zetta
