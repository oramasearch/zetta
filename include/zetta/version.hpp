// SPDX-License-Identifier: MIT

#pragma once

namespace zetta {

// Keep in sync with project(... VERSION ...) in CMakeLists.txt, info.version in
// schemas/openapi.json and docs/releases/v<version>.md; tests/test_release.cpp holds them together.
inline constexpr const char* kVersion = "0.6.0";

}  // namespace zetta
