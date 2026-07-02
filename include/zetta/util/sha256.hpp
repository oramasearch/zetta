#pragma once

#include <string>
#include <string_view>

namespace zetta {

// Lowercase-hex SHA-256 digest of the given bytes. Used to fingerprint large
// object bodies so a transferred file can be verified against the hash recorded
// in the replicated metadata.
std::string sha256_hex(std::string_view bytes);

}  // namespace zetta
