#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zetta::util {

// Lowercase-hex SHA-256 digest of the given bytes. Used to fingerprint large
// object bodies so a transferred file can be verified against the hash recorded
// in the replicated metadata.
std::string sha256_hex(std::string_view bytes);

// The same digest computed over bytes given in pieces, so a body larger than
// memory wants (a model's weights sent in chunks, lake#690) is hashed as it
// streams. update() any number of times, then hex() once.
class Sha256 {
public:
    Sha256();
    void update(std::string_view bytes);
    std::string hex();  // finishes the digest; call once

private:
    void block(const unsigned char* data);
    std::array<std::uint32_t, 8> hash_;
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

// The digest of a file's contents, read 1 MiB at a time; nullopt when it
// cannot be read.
std::optional<std::string> sha256_file_hex(const std::filesystem::path& path);

}  // namespace zetta::util
