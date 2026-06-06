cmake_minimum_required(VERSION 3.28)

# zetta's coroutine stack (Capy/Corosio, vendored under modules/) needs
# Clang 17+ — Apple's system Clang 15 is too old. On macOS, build with Homebrew
# LLVM clang (`brew install llvm`) when the user hasn't chosen a compiler. This
# must run before project(). Opt out with -DZETTA_USE_SYSTEM_COMPILER=ON or by
# passing -DCMAKE_CXX_COMPILER=...
if(APPLE AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT ZETTA_USE_SYSTEM_COMPILER
   AND EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
  set(CMAKE_C_COMPILER   "/opt/homebrew/opt/llvm/bin/clang"   CACHE FILEPATH "")
  set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++" CACHE FILEPATH "")
  # Homebrew clang doesn't assume Xcode's SDK; point it at the active one so
  # libc++ can find the C headers (time.h, etc.).
  if(NOT CMAKE_OSX_SYSROOT)
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE ZETTA_SDKROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_OSX_SYSROOT "${ZETTA_SDKROOT}" CACHE PATH "")
  endif()
endif()

project(zetta
  VERSION 0.0.1
  DESCRIPTION "A database; runs headless or with an SDL3 + Dear ImGui UI"
  LANGUAGES C CXX
)

# When built by Homebrew clang, run against its newer libc++ (its headers are
# ahead of the system dylib); link + rpath the matching runtime.
if(APPLE AND CMAKE_CXX_COMPILER MATCHES "/opt/homebrew/opt/llvm/")
  add_link_options(-L/opt/homebrew/opt/llvm/lib/c++
                   -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++)
endif()

# We don't use C++20 modules; skip the clang-scan-deps pass.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# -----------------------------------------------------------------------------
# Project-wide settings
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Use ccache as the compiler launcher when available — turns rebuilds after
# `cmake --build build` clean / fresh checkouts from minutes into seconds.
# Honors CMAKE_C/CXX_COMPILER_LAUNCHER if already set (e.g. by the user).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  if(NOT CMAKE_C_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  endif()
  if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  endif()
  message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

if(APPLE)
  # Targets macOS 12+. Bump if a dep requires newer (SDL3 + ImGui both fine
  # at 12+). The OSX_ARCHITECTURES default = native; override with
  # -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal build.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
endif()

# Quiet some clang warnings emitted by SDL/ImGui internals — we want a clean
# build for OUR code, not theirs.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(
    -Wno-deprecated-declarations  # SDL3 emits some on macOS for older APIs
  )
endif()

# -----------------------------------------------------------------------------
# Fetched dependencies (FetchContent — no brew install ritual needed)
# -----------------------------------------------------------------------------

include(FetchContent)

# --- SDL3 -------------------------------------------------------------------
# Window + input + GPU/renderer on macOS (Metal-backed by default in SDL3).
# Static so we ship a single binary; no .dylib next to the executable.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.2.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

# --- Dear ImGui --------------------------------------------------------------
# Dear ImGui doesn't ship a CMakeLists.txt; fetch the source and build our
# own static-library target with the SDL3 + SDL_Renderer backend files.
# FetchContent_MakeAvailable skips add_subdirectory silently when no
# CMakeLists.txt is present at the root, which is exactly what we want
# (CMake 4.2+ behavior; the older FetchContent_Populate is deprecated).
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  # std::string helpers for InputText — so we never touch char[] buffers.
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)

target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${imgui_SOURCE_DIR}/misc/cpp
)

target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

# Silence ImGui's own warnings on our compiler — they're upstream concerns.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(imgui PRIVATE
    -Wno-nontrivial-memaccess
    -Wno-deprecated-enum-enum-conversion
  )
endif()

# --- nlohmann/json -----------------------------------------------------------
# Header-only JSON. The first storage engine (JsonStore) holds nlohmann::json
# documents. Turn off its tests/install to keep the build graph small.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# -----------------------------------------------------------------------------
# zetta-core (the headless database node — no SDL/ImGui dependency)
# -----------------------------------------------------------------------------
# Everything a node needs to run without a display lives here: the node
# lifecycle today, the storage + query engine later. The GUI links it; the
# headless runner links it; keeping SDL/ImGui out of this target is the
# guarantee that a node can run on a headless server.

# -----------------------------------------------------------------------------
# Vendored modules (git submodules under modules/)
# -----------------------------------------------------------------------------
# zetta is a "bridge"-style repo: third-party dependencies we want pinned and
# locally readable (vs FetchContent) live as git submodules in modules/. Capy
# is the C++ Alliance coroutine foundation; Corosio (networking, also vendored)
# is wired in when zetta grows a server. Both need Clang 17+ (see the
# pre-project() toolchain block above).
set(BOOST_CAPY_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_BENCH    OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/capy EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# zetta-core (the headless database node — no SDL/ImGui dependency)
# -----------------------------------------------------------------------------
add_library(zetta-core STATIC
  src/core/node.cpp
  src/core/database.cpp
  src/core/table.cpp
  src/core/jsonl_store.cpp
  src/core/uuid.cpp
  src/core/duration.cpp
  src/core/bytes.cpp
  src/core/large_object.cpp
  src/core/import.cpp
  src/core/query/postgres.cpp
  src/core/query/executor.cpp
  src/core/query/service.cpp
  src/core/headless.cpp
  src/core/signals.cpp
)

target_compile_features(zetta-core PUBLIC cxx_std_26)

target_include_directories(zetta-core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

# PUBLIC: zetta::Node's interface exposes nlohmann::json (add_object) and the
# storage headers include it, so consumers (the GUI) get it transitively.
# Boost::capy backs the zetta::runtime async seam (include/zetta/runtime.hpp), also PUBLIC.
target_link_libraries(zetta-core PUBLIC nlohmann_json::nlohmann_json Boost::capy)

# -----------------------------------------------------------------------------
# zetta (the executable: headless core + the SDL3 + Dear ImGui headed UI)
# -----------------------------------------------------------------------------

add_executable(zetta
  src/main.cpp
  src/gui/app.cpp
)

target_compile_features(zetta PRIVATE cxx_std_26)

target_include_directories(zetta PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(zetta PRIVATE
  zetta-core
  imgui
  SDL3::SDL3-static
)

# Embed DroidSans.ttf (ships inside the FetchContent'd imgui tree) so the
# GUI can rasterize a real TTF at framebuffer-pixel density on HiDPI
# displays. ImGui's default ProggyClean is a 13px bitmap font that goes
# blurry the moment SDL_SetRenderScale upscales it; AddFontFromMemoryTTF
# with RasterizerDensity = pixel_density keeps text crisp.
set(ZETTA_FONT_SRC   "${imgui_SOURCE_DIR}/misc/fonts/DroidSans.ttf")
set(ZETTA_FONT_HPP   "${CMAKE_BINARY_DIR}/generated/zetta_font_embedded.hpp")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT  "${ZETTA_FONT_HPP}"
  COMMAND ${CMAKE_COMMAND}
          -DIN_FILE=${ZETTA_FONT_SRC}
          -DOUT_FILE=${ZETTA_FONT_HPP}
          -DSYMBOL=kDroidSans
          -P ${CMAKE_SOURCE_DIR}/cmake/embed_font.cmake
  DEPENDS ${ZETTA_FONT_SRC} ${CMAKE_SOURCE_DIR}/cmake/embed_font.cmake
  COMMENT "Embedding DroidSans.ttf into zetta_font_embedded.hpp"
)
add_custom_target(zetta-font-embed ALL DEPENDS "${ZETTA_FONT_HPP}")
add_dependencies(zetta zetta-font-embed)
target_include_directories(zetta PRIVATE "${CMAKE_BINARY_DIR}/generated")

set_target_properties(zetta PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# -----------------------------------------------------------------------------
# Tests (GoogleTest via FetchContent + CTest)
# -----------------------------------------------------------------------------
option(ZETTA_BUILD_TESTS "Build zetta unit tests" ON)
if(ZETTA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
