cmake_minimum_required(VERSION 3.28)

# zetta is C++26 and its coroutine stack (Capy/Corosio) needs Clang 17+ — older
# system compilers (Apple Clang 15, Ubuntu's GCC 13) can't build it. When the
# user hasn't chosen a compiler, pick a suitable one. This must run before
# project(). Opt out with -DZETTA_USE_SYSTEM_COMPILER=ON or by passing
# -DCMAKE_CXX_COMPILER=... (e.g. clang++-19, or g++-14 which also does C++26).
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT ZETTA_USE_SYSTEM_COMPILER)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
     AND EXISTS "/opt/homebrew/opt/llvm/bin/clang++")
    # macOS: Homebrew LLVM clang (`brew install llvm`).
    set(CMAKE_C_COMPILER   "/opt/homebrew/opt/llvm/bin/clang"   CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++" CACHE FILEPATH "")
    # Homebrew clang doesn't assume Xcode's SDK; point it at the active one so
    # libc++ can find the C headers (time.h, etc.).
    if(NOT CMAKE_OSX_SYSROOT)
      execute_process(COMMAND xcrun --show-sdk-path
                      OUTPUT_VARIABLE ZETTA_SDKROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
      set(CMAKE_OSX_SYSROOT "${ZETTA_SDKROOT}" CACHE PATH "")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    # Linux: prefer the newest installed clang >= 17 (e.g. clang++-18 from apt).
    # install-deps.sh provides one. Falls through to the system default (and a
    # clear cxx_std_26 error) if none is found — then pass -DCMAKE_CXX_COMPILER.
    foreach(ZETTA_VER 21 20 19 18 17)
      find_program(ZETTA_CLANGXX_${ZETTA_VER} "clang++-${ZETTA_VER}")
      if(ZETTA_CLANGXX_${ZETTA_VER})
        set(CMAKE_CXX_COMPILER "${ZETTA_CLANGXX_${ZETTA_VER}}" CACHE FILEPATH "")
        find_program(ZETTA_CLANGC_${ZETTA_VER} "clang-${ZETTA_VER}")
        if(ZETTA_CLANGC_${ZETTA_VER})
          set(CMAKE_C_COMPILER "${ZETTA_CLANGC_${ZETTA_VER}}" CACHE FILEPATH "")
        endif()
        break()
      endif()
    endforeach()
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

# nlohmann/json (the storage engine + query layer use nlohmann::json) is
# provided by automerge-cpp's vendored copy — it adds the `nlohmann_json` target
# unconditionally (modules/automerge-cpp/upstream/json), so we consume that one
# rather than FetchContent a second, clashing copy. See the modules section.

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

# Corosio — coroutine-native networking (needs Capy above). Its optional TLS
# provider wants OpenSSL, which is keg-only on Homebrew.
if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3)
endif()
set(BOOST_COROSIO_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_PERF     OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/corosio EXCLUDE_FROM_ALL)

# automerge-cpp — a CRDT (conflict-free replicated data type) for the replicated
# DDL/metadata/rows. From-scratch C++23, no Rust. It vendors nlohmann/json under
# upstream/json and adds the `nlohmann_json` target, which the rest of zetta-core
# also links (see the json note above). Needs its nested submodules initialized.
set(AUTOMERGE_CPP_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(AUTOMERGE_CPP_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(AUTOMERGE_CPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(AUTOMERGE_CPP_BUILD_FUZZ       OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/automerge-cpp EXCLUDE_FROM_ALL)

# json-schema-validator (pboettch) — validates stored/replicated JSON against a
# JSON Schema at the storage boundary (the Run record; see src/core/run.cpp). It
# builds on nlohmann/json, so it MUST be declared AFTER automerge-cpp above (which
# provides the `nlohmann_json::nlohmann_json` target) — that way it reuses our
# json target instead of fetching a second, clashing copy. Used only by
# zetta-core's adapters, so it's linked PRIVATE (stays out of the public surface).
include(FetchContent)
set(JSON_VALIDATOR_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  json_schema_validator
  GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
  GIT_TAG        2.4.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json_schema_validator)

# Match zetta-core's libc++ ABI on the validator. zetta-core compiles with
# -fexperimental-library (propagated PUBLIC from Boost::capy, which links
# libc++experimental); the validator does not. A std:: exception thrown inside
# the validator and caught in zetta-core (run.cpp) then fails its typeinfo match
# across the experimental/non-experimental boundary and std::terminate fires.
# Compiling the validator with the same flag keeps the exception ABI identical.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fexperimental-library ZETTA_HAS_EXPERIMENTAL_LIBRARY)
if(ZETTA_HAS_EXPERIMENTAL_LIBRARY)
  target_compile_options(nlohmann_json_schema_validator PRIVATE -fexperimental-library)
endif()

# -----------------------------------------------------------------------------
# zetta-compute (the accelerated-compute backends — no SDL/ImGui, no CUDA leak)
# -----------------------------------------------------------------------------
# The zetta::compute ComputeDevice seam (ADR-27 / ARC-9) and its backends live in
# their own static library, linked into zetta-core like any other adapter. This is
# where the optional CUDA backend lands: CUDA headers + kernels stay inside this
# target's .cu translation unit and never reach zetta-core — the same discipline
# that keeps SDL/ImGui out of zetta-core. A CPU-only configure (dev Macs, CI)
# builds this library with the host stub only and needs no CUDA toolkit.

option(ZETTA_ENABLE_CUDA
  "Build the optional CUDA compute backend (requires the CUDA toolkit)" OFF)

set(ZETTA_COMPUTE_SOURCES
  src/core/compute/device_buffer.cpp
  src/core/compute/host_stub_device.cpp)

if(ZETTA_ENABLE_CUDA)
  # Detect the toolkit (CUDA::cudart + the Thrust headers) and turn on the CUDA
  # language only when the backend is requested — a CPU-only configure never
  # touches CUDA. REQUIRED so an explicit opt-in fails loudly when the toolkit is
  # absent, rather than silently dropping the backend.
  find_package(CUDAToolkit REQUIRED)
  enable_language(CUDA)
  list(APPEND ZETTA_COMPUTE_SOURCES src/core/compute/cuda_device.cu)
  message(STATUS "CUDA compute backend: ENABLED (CUDAToolkit ${CUDAToolkit_VERSION})")
else()
  message(STATUS "CUDA compute backend: disabled (CPU-only build). "
                 "Enable with -DZETTA_ENABLE_CUDA=ON on a host with the CUDA toolkit.")
endif()

add_library(zetta-compute STATIC ${ZETTA_COMPUTE_SOURCES})

target_compile_features(zetta-compute PUBLIC cxx_std_26)
target_include_directories(zetta-compute PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Boost::capy is linked PUBLIC for the ABI it propagates as much as the thread
# pool the forthcoming CpuDevice (#360) will use: it brings -fexperimental-library,
# so this library's libc++ ABI matches zetta-core's. ComputeStatus carries a
# std::string across the zetta-compute -> zetta-core link boundary; a mismatched
# string ABI there is the same failure the validator note above guards against.
target_link_libraries(zetta-compute PUBLIC Boost::capy)

if(ZETTA_ENABLE_CUDA)
  # nvcc compiles the .cu at a CUDA-supported standard (C++26 is beyond nvcc); the
  # public compute headers it includes only need C++20 (std::span). Use the same
  # host compiler as the rest of zetta so the host-side libc++ ABI lines up across
  # the link boundary (override with -DCMAKE_CUDA_HOST_COMPILER=... if needed).
  set_target_properties(zetta-compute PROPERTIES
    CUDA_STANDARD 20
    CUDA_STANDARD_REQUIRED ON)
  if(NOT CMAKE_CUDA_HOST_COMPILER)
    set_target_properties(zetta-compute PROPERTIES
      CUDA_HOST_COMPILER "${CMAKE_CXX_COMPILER}")
  endif()
  # ZETTA_WITH_CUDA is PUBLIC so consumers (the device registry #362, tests) guard
  # their use of CudaDevice on it. cudart is PRIVATE so it links into the final
  # binary without leaking the CUDA include directories into zetta-core (a static-
  # library PRIVATE dependency propagates to consumers as link-only).
  target_compile_definitions(zetta-compute PUBLIC ZETTA_WITH_CUDA=1)
  target_link_libraries(zetta-compute PRIVATE CUDA::cudart)
  # Match zetta-core's libc++ experimental ABI on the .cu's host compilation when
  # the host compiler is Clang (the toolchain zetta selects).
  if(ZETTA_HAS_EXPERIMENTAL_LIBRARY)
    target_compile_options(zetta-compute PRIVATE
      $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fexperimental-library>)
  endif()
endif()

# -----------------------------------------------------------------------------
# zetta-core (the headless database node — no SDL/ImGui dependency)
# -----------------------------------------------------------------------------
add_library(zetta-core STATIC
  src/core/cluster.cpp
  src/core/signal.cpp
  src/core/collectives.cpp
  src/core/replication.cpp
  src/core/node.cpp
  src/core/database.cpp
  src/core/project.cpp
  src/core/project_input.cpp
  src/core/project_graph.cpp
  src/core/deployment_target.cpp
  src/core/deployment.cpp
  src/core/deployment_controller.cpp
  src/core/node_capability.cpp
  src/core/provisioner.cpp
  src/core/local_provisioner.cpp
  src/core/command_runner.cpp
  src/core/azure_provisioner.cpp
  src/core/azure_cli_control_plane.cpp
  src/core/gcp_provisioner.cpp
  src/core/gcp_cli_control_plane.cpp
  src/core/burst_identity.cpp
  src/core/scaling_policy.cpp
  src/core/burst_approval_gate.cpp
  src/core/solution_catalog.cpp
  src/core/run.cpp
  src/core/model.cpp
  src/core/compute_job.cpp
  src/core/principal.cpp
  src/core/access_key.cpp
  src/core/scope.cpp
  src/core/capability.cpp
  src/core/audit_event.cpp
  src/core/table.cpp
  src/core/jsonl_store.cpp
  src/core/uuid.cpp
  src/core/sha256.cpp
  src/core/timestamp.cpp
  src/core/validation.cpp
  src/core/gate.cpp
  src/core/graph_checks.cpp
  src/core/checks/entity_resolution_check.cpp
  src/core/executor.cpp
  src/core/intent_compiler.cpp
  src/core/duration.cpp
  src/core/bytes.cpp
  src/core/large_object.cpp
  src/core/replicated_blob_store.cpp
  src/core/import.cpp
  src/core/churn_dataset.cpp
  src/core/n8n_import.cpp
  src/core/n8n_compile.cpp
  src/core/ir/execution_graph.cpp
  src/core/ir/serialization.cpp
  src/core/compute/cpu_device.cpp
  src/core/compute/device_registry.cpp
  src/core/compute/work_stealing_scheduler.cpp
  src/core/compute/distributed_scheduler.cpp
  src/core/runtime.cpp
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
# zetta-compute houses the zetta::compute ComputeDevice backends (its public
# headers live in include/zetta/compute and are part of zetta-core's surface);
# PUBLIC so consumers linking zetta-core get the backend symbols + the optional
# ZETTA_WITH_CUDA define, while CUDA headers stay confined to that library.
target_link_libraries(zetta-core PUBLIC
  nlohmann_json::nlohmann_json Boost::capy zetta-compute)
# Corosio (networking), automerge-cpp (the replication CRDT), and the JSON-Schema
# validator (boundary validation in src/core/run.cpp) are used only by zetta-core's
# adapters — PRIVATE so they stay out of zetta-core's public surface and the GUI.
target_link_libraries(zetta-core PRIVATE
  Boost::corosio
  automerge-cpp::automerge-cpp
  nlohmann_json_schema_validator)

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
