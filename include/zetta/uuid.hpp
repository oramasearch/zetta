#pragma once

#include <string>

namespace zetta {

// A random (version 4) UUID in canonical 8-4-4-4-12 lowercase-hex form,
// e.g. "f47ac10b-58cc-4372-a567-0e02b2c3d479". Used as database/table folder
// names on disk.
std::string new_uuid();

}  // namespace zetta
