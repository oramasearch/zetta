// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

namespace zetta::util {

// The raw 20-byte SHA-1 digest of `bytes` (FIPS 180-4).
//
// SHA-1 is broken for collision resistance and must never be used to protect
// anything new. It exists here only because a wire protocol zetta speaks still
// requires it: MySQL's `mysql_native_password` challenge-response
// (src/core/connect/mysql_client.cpp, #895). Content fingerprints use SHA-256
// (sha256.hpp).
std::string sha1_digest(std::string_view bytes);

}  // namespace zetta::util
