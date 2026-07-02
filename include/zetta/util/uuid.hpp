#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace zetta {

// A random (version 4) UUID in canonical 8-4-4-4-12 lowercase-hex form,
// e.g. "f47ac10b-58cc-4372-a567-0e02b2c3d479". Used as database/table folder
// names on disk and as a node's identity (cluster node id + replication actor).
std::string new_uuid();

// The 16 raw bytes of a canonical UUID string (hyphens ignored). Returns
// all-zero bytes if the string isn't a valid 32-hex-digit UUID. A UUID is
// exactly 16 bytes — the size of an Automerge actor id.
std::array<std::byte, 16> uuid_to_bytes(std::string_view uuid);

}  // namespace zetta
