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
  VERSION 0.4.0
  DESCRIPTION "A database; runs headless or with an SDL3 + Dear ImGui UI"
  LANGUAGES C CXX
)

# When built by Homebrew clang (the default `llvm` keg or a versioned
# `llvm@NN`), run against its newer libc++ (its headers are ahead of the
# system dylib); link + rpath the matching runtime. Because we run against
# Homebrew's own libc++, its headers' availability annotations — which gate
# symbols on the macOS *system* dylib, e.g. floating-point std::from_chars
# ("introduced in macOS 26.0" since LLVM 21.1.8) — are false alarms here:
# disable them.
if(APPLE AND CMAKE_CXX_COMPILER MATCHES "^(/opt/homebrew/opt/llvm[^/]*)/")
  set(ZETTA_HOMEBREW_LLVM_PREFIX "${CMAKE_MATCH_1}")
  # Homebrew's libc++abi must unwind with Homebrew's libunwind, not the system
  # one (the keg's own caveat). Linked against the system unwinder, a thrown
  # exception is sometimes not caught by a matching handler and std::terminate()s
  # instead — a catch that works in a toy program and fails in a real frame. It
  # made the registry's exception containment (#1007) a no-op on macOS, and is
  # likely also what the postgres.cpp "-O1 miscompile" note below describes (that
  # workaround is left in place until it is re-verified without it).
  add_link_options(-L${ZETTA_HOMEBREW_LLVM_PREFIX}/lib/c++
                   -Wl,-rpath,${ZETTA_HOMEBREW_LLVM_PREFIX}/lib/c++
                   -L${ZETTA_HOMEBREW_LLVM_PREFIX}/lib/unwind
                   -Wl,-rpath,${ZETTA_HOMEBREW_LLVM_PREFIX}/lib/unwind
                   -lunwind)
  add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY)
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

# Build only the reusable transport when Zetta is consumed as a dependency.
# The mode avoids configuring the product, GUI, database, tests, and unrelated
# vendored modules. See docs/cluster/transport-library.md.
option(ZETTA_TRANSPORT_ONLY
  "Configure only the reusable Corosio mutual-TLS transport" OFF)

# Capy + Corosio are pinned submodules. The OpenSSL provider supplies the TLS
# stream while zetta_transport validates PEM credentials through libcrypto.
set(BOOST_CAPY_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_CAPY_BUILD_BENCH    OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/capy EXCLUDE_FROM_ALL)

if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)
set(BOOST_COROSIO_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_PERF     OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/corosio EXCLUDE_FROM_ALL)

add_library(zetta_transport
  src/transport/security.cpp
  src/transport/framed_stream.cpp)
add_library(zetta::transport ALIAS zetta_transport)
target_compile_features(zetta_transport PUBLIC cxx_std_26)
target_include_directories(zetta_transport PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zetta_transport
  PUBLIC Boost::capy Boost::corosio Boost::corosio_openssl
  PRIVATE OpenSSL::Crypto)

if(ZETTA_TRANSPORT_ONLY)
  return()
endif()

find_package(ZLIB REQUIRED)

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
# Capy, Corosio, OpenSSL, and zetta_transport were configured above so the same
# targets serve both the transport-only consumer mode and the full product.

# --- The API surface's HTTP stack (docs/api/registry-design.md, #615) --------
# beast2 (coroutine-native HTTP/1 server) + http (sans-I/O HTTP + router) ride
# the SAME capy/corosio stack vendored above — one async model in the binary
# (docs/research/beast2-http.md). Both are pinned C++ Alliance submodules like
# capy/corosio. Their remaining Boost deps (json, url + header-only
# transitives) come from the official boost-cmake release via FetchContent
# (the SDL3/GoogleTest precedent): as SUBDIRECTORIES the two libraries do NOT
# self-resolve Boost — they expect the Boost:: targets to exist, so this block
# MUST come before them (capy/corosio already provide Boost::capy/corosio).
set(BOOST_INCLUDE_LIBRARIES json url assert config core describe endian mp11 static_assert system throw_exception type_traits variant2 winapi)
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(
  boost_core_libs
  URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
  URL_HASH ""
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(boost_core_libs)

set(BOOST_HTTP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_HTTP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/http EXCLUDE_FROM_ALL)

set(BOOST_BEAST2_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_BEAST2_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/beast2 EXCLUDE_FROM_ALL)
# Beast2 compiles its HTTPS server into boost_beast2, so a shared Beast2
# library must resolve Corosio's OpenSSL stream symbols itself (#1096).
target_link_libraries(boost_beast2 PRIVATE Boost::corosio_openssl)

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
# JSON Schema at the storage boundary (the Run record; see src/core/pipeline/run.cpp). It
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

# oramacore-cpp — the native C++23 full-text (BM25F) engine (modules/oramacore-cpp)
# that backs the OramaCoreFulltextIndex search backend (ENT-39 / ENT-40, #527). It
# is a *private* submodule, so a checkout that did not (or could not) initialize it
# must still configure + build + run green: guard on the submodule's presence and
# link the engine only when it is there. When absent, the backend degrades to
# `Unsupported` (see src/core/index/oramacore_fulltext_index.cpp). CPU-only, no GPU.
set(ZETTA_ORAMACORE_DIR "${CMAKE_SOURCE_DIR}/modules/oramacore-cpp")
if(EXISTS "${ZETTA_ORAMACORE_DIR}/CMakeLists.txt")
  # Don't build oramacore-cpp's own Catch2 tests/bench (zetta drives its own
  # GoogleTest suite). Its tests gate on BUILD_TESTING via include(CTest); zetta
  # uses its own ZETTA_BUILD_TESTS, not BUILD_TESTING, so forcing it OFF is safe.
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ORAMACORE_CPP_BENCH OFF CACHE BOOL "" FORCE)
  add_subdirectory("${ZETTA_ORAMACORE_DIR}" EXCLUDE_FROM_ALL)
  # Match zetta-core's libc++ ABI on the engine. zetta-core compiles with
  # -fexperimental-library (propagated PUBLIC from Boost::capy); the engine does
  # not. Std containers cross the zetta-core -> oramacore-cpp link boundary, so
  # compile it with the same flag to keep the libc++ ABI identical (the same guard
  # the JSON-Schema validator gets above).
  if(ZETTA_HAS_EXPERIMENTAL_LIBRARY)
    target_compile_options(oramacore-cpp PRIVATE -fexperimental-library)
  endif()
  set(ZETTA_WITH_ORAMACORE ON)
  message(STATUS "oramacore-cpp full-text index backend: ENABLED")
else()
  set(ZETTA_WITH_ORAMACORE OFF)
  message(STATUS "oramacore-cpp full-text index backend: disabled "
                 "(modules/oramacore-cpp not initialized — "
                 "`git submodule update --init modules/oramacore-cpp` to enable)")
endif()

# libpqxx — the Postgres source connector (#892). A public-upstream submodule
# (modules/libpqxx, jtv/libpqxx) on **libpq**. Guarded on presence so a checkout
# without it still configures + builds + runs green (ZETTA_WITH_POSTGRES) — the
# same discipline as oramacore-cpp above. The blocking libpq calls are wrapped in
# runtime::offload so zetta's threading is never disturbed; the adapter
# (src/core/api/postgres_protocol.cpp) is the one TU that names libpqxx and links
# into zetta-api, so **zetta-core links no libpq** (the outbound-transport
# boundary). ADR-17 exception (vendor driver) — a deliberate, documented decision.
set(ZETTA_LIBPQXX_DIR "${CMAKE_SOURCE_DIR}/modules/libpqxx")
if(EXISTS "${ZETTA_LIBPQXX_DIR}/CMakeLists.txt")
  # libpqxx's src/CMakeLists.txt does find_package(PostgreSQL REQUIRED). On macOS
  # Homebrew keeps libpq keg-only, so add the hint prefixes; on Ubuntu/the build
  # box libpq-dev is found without help.
  if(APPLE)
    foreach(pg_prefix /opt/homebrew/opt/libpq /usr/local/opt/libpq
                      /opt/homebrew/opt/postgresql@17)
      if(EXISTS "${pg_prefix}")
        list(APPEND CMAKE_PREFIX_PATH "${pg_prefix}")
      endif()
    endforeach()
  endif()
  set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)  # don't build libpqxx's own tests
  add_subdirectory("${ZETTA_LIBPQXX_DIR}" EXCLUDE_FROM_ALL)
  # Match zetta-core's libc++ ABI (std containers cross the link boundary), same
  # as the oramacore + json-schema-validator guards above.
  if(ZETTA_HAS_EXPERIMENTAL_LIBRARY)
    target_compile_options(pqxx PRIVATE -fexperimental-library)
  endif()
  set(ZETTA_WITH_POSTGRES ON)
  message(STATUS "Postgres source connector (libpqxx): ENABLED")
else()
  set(ZETTA_WITH_POSTGRES OFF)
  message(STATUS "Postgres source connector: disabled "
                 "(modules/libpqxx not initialized — "
                 "`git submodule update --init modules/libpqxx` to enable)")
endif()

# -----------------------------------------------------------------------------
# zetta-compute (the accelerated-compute backends — no SDL/ImGui, no CUDA leak)
# -----------------------------------------------------------------------------
# The zetta::compute ComputeDevice interface (ADR-27 / ARC-9) and its backends live in
# their own static library, linked into zetta-core like any other adapter. This is
# where the optional CUDA backend lands: CUDA headers + kernels stay inside this
# target's .cu translation unit and never reach zetta-core — the same discipline
# that keeps SDL/ImGui out of zetta-core. A CPU-only configure (dev Macs, CI)
# builds this library with the host stub only and needs no CUDA toolkit.

option(ZETTA_ENABLE_CUDA
  "Build the optional CUDA compute backend (requires the CUDA toolkit)" OFF)
option(ZETTA_ENABLE_MLX
  "Build the optional MLX (Apple-Silicon/Metal) compute backend (requires libmlx)" OFF)

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
  list(APPEND ZETTA_COMPUTE_SOURCES src/core/compute/cuda_device.cu
                                    src/core/compute/cuda_transformer.cu
                                    src/core/compute/cuda_resident.cpp
                                    src/core/compute/cuda_qwen_model.cpp)
  message(STATUS "CUDA compute backend: ENABLED (CUDAToolkit ${CUDAToolkit_VERSION})")
else()
  message(STATUS "CUDA compute backend: disabled (CPU-only build). "
                 "Enable with -DZETTA_ENABLE_CUDA=ON on a host with the CUDA toolkit.")
endif()

if(ZETTA_ENABLE_MLX)
  # MLX is a normal C++ library (no separate language, unlike CUDA): find its
  # header + shared lib and add the backend TU. REQUIRED so an explicit opt-in
  # fails loudly when libmlx is absent rather than silently dropping the backend.
  find_path(MLX_INCLUDE_DIR mlx/mlx.h REQUIRED)
  find_library(MLX_LIBRARY mlx REQUIRED)
  list(APPEND ZETTA_COMPUTE_SOURCES
    src/core/compute/mlx_device.cpp
    src/core/compute/mlx_transformer.cpp
    src/core/compute/mlx_qwen.cpp
    src/core/compute/mlx_qwen_inference_install.cpp
    src/core/compute/safetensors.cpp)
  message(STATUS "MLX compute backend: ENABLED (${MLX_LIBRARY})")
else()
  message(STATUS "MLX compute backend: disabled (CPU-only build). "
                 "Enable with -DZETTA_ENABLE_MLX=ON on a host with libmlx (macOS).")
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
  # Only nvcc has a separate host compiler. It also enforces a version whitelist that
  # our toolchain outruns: CUDA 12.8's nvcc rejects host Clang >= 20 outright
  # ("unsupported clang version! clang version must be less than 20"), so pairing it
  # with the Clang 22 the rest of the build uses is not possible. Two ways out, and
  # CMAKE_CUDA_COMPILER decides which:
  #   * nvcc  -> pin an older host Clang (-DCMAKE_CUDA_HOST_COMPILER=clang++-19), or
  #   * Clang -> let Clang compile the .cu itself (-DCMAKE_CUDA_COMPILER=clang++-22),
  #     which needs no host compiler at all and keeps ONE compiler across the build.
  if(CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA" AND NOT CMAKE_CUDA_HOST_COMPILER)
    set_target_properties(zetta-compute PROPERTIES
      CUDA_HOST_COMPILER "${CMAKE_CXX_COMPILER}")
  endif()
  # ZETTA_WITH_CUDA is PUBLIC so consumers (the device registry #362, tests) guard
  # their use of CudaDevice on it. cudart is PRIVATE so it links into the final
  # binary without leaking the CUDA include directories into zetta-core (a static-
  # library PRIVATE dependency propagates to consumers as link-only).
  target_compile_definitions(zetta-compute PUBLIC ZETTA_WITH_CUDA=1)
  # cuBLAS provides the tuned GEMM (#929). The hand-written kernel stays as the fallback for
  # dtypes cuBLAS does not cover and as the conformance reference.
  target_link_libraries(zetta-compute PRIVATE CUDA::cudart CUDA::cublas)
  # Match zetta-core's libc++ experimental ABI on the .cu's host compilation. How the
  # flag is spelled depends on which compiler owns the .cu: nvcc forwards to its host
  # compiler via -Xcompiler, whereas Clang IS the host compiler and takes the flag
  # directly. Getting this wrong is not a subtle failure -- an unguarded -Xcompiler
  # reaches nvcc's DEFAULT host compiler (gcc), which rejects the clang-only flag with
  # "unrecognized command-line option '-fexperimental-library'".
  if(ZETTA_HAS_EXPERIMENTAL_LIBRARY)
    if(CMAKE_CUDA_COMPILER_ID STREQUAL "NVIDIA")
      target_compile_options(zetta-compute PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fexperimental-library>)
    elseif(CMAKE_CUDA_COMPILER_ID MATCHES "Clang")
      target_compile_options(zetta-compute PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-fexperimental-library>)
    endif()
  endif()
endif()

if(ZETTA_ENABLE_MLX)
  # ZETTA_WITH_MLX is PUBLIC so consumers (the device registry, tests) guard their
  # use of MlxDevice on it. The MLX headers + libmlx are PRIVATE so they link into
  # the final binary without leaking into zetta-core's public surface. On macOS
  # libmlx pulls the Metal/Foundation frameworks it was built against.
  target_compile_definitions(zetta-compute PUBLIC ZETTA_WITH_MLX=1)
  target_include_directories(zetta-compute PRIVATE ${MLX_INCLUDE_DIR})
  # nlohmann/json (header-only) for the safetensors header; the include path comes
  # via the interface target set up above.
  target_link_libraries(zetta-compute PRIVATE ${MLX_LIBRARY} nlohmann_json)
  if(APPLE)
    target_link_libraries(zetta-compute PRIVATE
      "-framework Metal" "-framework Foundation" "-framework QuartzCore"
      "-framework Accelerate")
  endif()
endif()

# -----------------------------------------------------------------------------
# zetta-core (the headless database node — no SDL/ImGui dependency)
# -----------------------------------------------------------------------------
add_library(zetta-core STATIC
  src/core/memory/accounting.cpp
  src/core/pubsub/address.cpp
  src/core/pubsub/event.cpp
  src/core/pubsub/store.cpp
  src/core/pubsub/delivery.cpp
  src/core/pubsub/dispatcher.cpp
  src/core/pubsub/resources.cpp
  src/core/pubsub/topic.cpp
  src/core/pubsub/pubsub.cpp
  src/core/pubsub/node_integration.cpp
  src/core/observability/activity.cpp
  src/core/observability/trace.cpp
  src/core/observability/crash.cpp
  src/core/observability/feedback.cpp
  src/core/api/background.cpp
  src/core/api/bridge.cpp
  src/core/api/capabilities.cpp
  src/core/api/pubsub_capabilities.cpp
  src/core/api/run_metrics_capabilities.cpp
  src/core/api/record_capabilities.cpp
  src/core/api/network_capabilities.cpp
  src/core/api/object_capabilities.cpp
  src/core/api/storage_capabilities.cpp
  src/core/api/study_capabilities.cpp
  src/core/api/decision_policy_capabilities.cpp
  src/core/api/solution_capabilities.cpp
  src/core/api/action_capabilities.cpp
  src/core/api/run_proposal_capabilities.cpp
  src/core/api/cloud_capabilities.cpp
  src/core/api/llms.cpp
  src/core/api/mcp_docs.cpp
  src/core/api/mcp_prompts.cpp
  src/core/api/registry.cpp
  src/core/api/rest.cpp
  src/core/api/serving.cpp
  src/core/api/specs.cpp
  src/core/api/web_ui.cpp
  src/core/schemas/registry.cpp
  src/core/solutions/decision_policy.cpp
  src/core/solutions/action_integration.cpp
  src/core/cluster/cluster.cpp
  src/core/cluster/security.cpp
  src/core/cluster/signal.cpp
  src/core/cluster/collectives.cpp
  src/core/cluster/replication.cpp
  src/core/data/replication_strategy.cpp
  src/core/node.cpp
  src/core/cluster/node_metrics.cpp
  src/core/cluster/node_telemetry.cpp
  src/core/data/database.cpp
  src/core/pipeline/project.cpp
  src/core/pipeline/project_input.cpp
  src/core/pipeline/project_graph.cpp
  src/core/pipeline/deployment_target.cpp
  src/core/pipeline/deployment.cpp
  src/core/pipeline/deployment_controller.cpp
  src/core/cloud/node_capability.cpp
  src/core/cloud/node_bootstrap_env.cpp
  src/core/cloud/node_bootstrap_script.cpp
  src/core/cloud/provisioner.cpp
  src/core/cloud/provisioner_detect.cpp
  src/core/cloud/add_nodes.cpp
  src/core/cloud/local_provisioner.cpp
  src/core/cloud/provisioner_fleet.cpp
  src/core/cloud/command_runner.cpp
  src/core/cloud/secret_file.cpp
  src/core/cloud/azure_provisioner.cpp
  src/core/cloud/azure_cli_control_plane.cpp
  src/core/cloud/aws_provisioner.cpp
  src/core/cloud/aws_cli_control_plane.cpp
  src/core/cloud/gcp_provisioner.cpp
  src/core/cloud/gcp_cli_control_plane.cpp
  src/core/cloud/oci_provisioner.cpp
  src/core/cloud/oci_cli_control_plane.cpp
  src/core/cloud/k8s_provisioner.cpp
  src/core/cloud/k8s_cli_control_plane.cpp
  src/core/cloud/bare_metal_provisioner.cpp
  src/core/cloud/bare_metal_cli_control_plane.cpp
  src/core/cloud/burst_identity.cpp
  src/core/compute/scaling_policy.cpp
  src/core/cloud/burst_approval_gate.cpp
  src/core/compute/job_autoscaler.cpp
  src/core/pipeline/solution_catalog.cpp
  src/core/cloud/cloud_provider.cpp
  src/core/cloud/cloud_inspector.cpp
  src/core/pipeline/run.cpp
  src/core/pipeline/run_proposal.cpp
  src/core/solutions/monitor.cpp
  src/core/solutions/roi_readout.cpp
  src/core/solutions/solution_templates.cpp
  src/core/solutions/solution_foundation.cpp
  src/core/solutions/solution_execution.cpp
  src/core/ml/ensemble.cpp
  src/core/ml/ensemble_methodology.cpp
  src/core/ml/ensemble_runner.cpp
  src/core/ml/ensemble_train_job.cpp
  src/core/ml/methodology.cpp
  src/core/ml/metric_names.cpp
  src/core/ml/study.cpp
  src/core/ml/methodology_optimization.cpp
  src/core/ml/methodology_table.cpp
  src/core/ml/workspace_methodology.cpp
  src/core/ml/workspace_study.cpp
  src/core/ml/solution_table.cpp
  src/core/ml/entity_resolution.cpp
  src/core/ml/entity_resolution_job.cpp
  src/core/ml/entity_resolution_methodology.cpp
  src/core/ml/graph.cpp
  src/core/ml/graph_job.cpp
  src/core/ml/graph_methodology.cpp
  src/core/ml/graph_runner.cpp
  src/core/ml/vision_methodology.cpp
  src/core/ml/document_methodology.cpp
  src/core/ml/document_extraction.cpp
  src/core/ml/speech_audio.cpp
  src/core/ml/speech_whisper.cpp
  src/core/ml/speech_train_job.cpp
  src/core/ml/speech_runner.cpp
  src/core/ml/speech_methodology.cpp
  src/core/ml/bandit_methodology.cpp
  src/core/ml/optimization.cpp
  src/core/ml/bayesian.cpp
  src/core/ml/bayesian_methodology.cpp
  src/core/ml/label_efficiency.cpp
  src/core/ml/label_efficiency_methodology.cpp
  src/core/ml/label_efficiency_runner.cpp
  src/core/ml/synthetic_private.cpp
  src/core/ml/synthetic_private_methodology.cpp
  src/core/ml/synthetic_private_job.cpp
  src/core/ml/streaming_methodology.cpp
  src/core/ml/model.cpp
  src/core/ml/survival.cpp
  src/core/ml/survival_methodology.cpp
  src/core/workspace/workspace_profile.cpp
  src/core/billing/plan.cpp
  src/core/solutions/experiment.cpp
  src/core/index/index.cpp
  src/core/index/index_registry.cpp
  src/core/index/index_step_executor.cpp
  src/core/index/in_memory_index_backend.cpp
  src/core/index/oramacore_fulltext_index.cpp
  src/core/index/distributed_fulltext.cpp
  src/core/index/search.cpp
  src/core/compute/compute_job.cpp
  src/core/data/dataset_manifest.cpp
  src/core/data/partition_scheduler.cpp
  src/core/data/dataset_partition.cpp
  src/core/data/fetch_affinity_placement.cpp
  src/core/data/rendezvous_placement.cpp
  src/core/data/partition_writer.cpp
  src/core/data/partition_rebalance.cpp
  src/core/data/promote_dataset.cpp
  src/core/governance/principal.cpp
  src/core/governance/access_key.cpp
  src/core/governance/scope.cpp
  src/core/governance/grant.cpp
  src/core/governance/capability.cpp
  src/core/governance/permission.cpp
  src/core/governance/audit_event.cpp
  src/core/governance/consent.cpp
  src/core/governance/credential_redaction.cpp
  src/core/governance/token_secret.cpp
  src/core/governance/user.cpp
  src/core/governance/role.cpp
  src/core/governance/token.cpp
  src/core/governance/user_role.cpp
  src/core/governance/policy.cpp
  src/core/solutions/audit_ledger.cpp
  src/core/data/table.cpp
  src/core/data/dataset_columns.cpp
  src/core/data/partitioner.cpp
  src/core/data/partition_body.cpp
  src/core/storage/lazy_row.cpp
  src/core/storage/jsonl_store.cpp
  src/core/storage/parquet_store.cpp
  src/core/util/private_directory.cpp
  src/core/util/uuid.cpp
  src/core/util/sha256.cpp
  src/core/util/timestamp.cpp
  src/core/pipeline/validation.cpp
  src/core/pipeline/gate.cpp
  src/core/pipeline/graph_checks.cpp
  src/core/pipeline/next_best_action_graph_check.cpp
  src/core/solutions/snapshot.cpp
  src/core/usecases/churn/entity_resolution_check.cpp
  src/core/usecases/churn/entity_resolution.cpp
  src/core/pipeline/executor.cpp
  src/core/pipeline/step_authority.cpp
  src/core/pipeline/run_progress.cpp
  src/core/usecases/churn/churn_step_executor.cpp
  src/core/usecases/churn/churn_solution.cpp
  src/core/pipeline/intent_compiler.cpp
  src/core/pipeline/graph_plan.cpp
  src/core/util/duration.cpp
  src/core/util/bytes.cpp
  src/core/data/large_object.cpp
  src/core/data/replicated_blob_store.cpp
  src/core/data/import.cpp
  src/core/data/virtual_table.cpp
  src/core/data/record_store.cpp
  src/core/network/app_credential.cpp
  src/core/network/records.cpp
  src/core/data/record_log.cpp
  src/core/data/metric_store.cpp
  src/core/data/object_store.cpp
  src/core/data/storage_census.cpp
  src/core/data/object_log.cpp
  src/core/data/add_dataset.cpp
  src/core/data/dataset_path.cpp
  src/core/data/import_path.cpp
  src/core/data/export_path.cpp
  src/core/data/categorical_encoding.cpp
  src/core/data/analyze_dataset.cpp
  src/core/data/featurize_plan.cpp
  src/core/ml/run_features.cpp
  src/core/connect/connection_store.cpp
  src/core/connect/credential_seal.cpp
  src/core/connect/connector.cpp
  src/core/connect/postgres_runner.cpp
  src/core/connect/distributed_fetch.cpp
  src/core/connect/distributed_ingest.cpp
  src/core/connect/snowflake_protocol.cpp
  src/core/connect/http_transport.cpp
  src/core/connect/model_fetch.cpp
  src/core/connect/destination_policy.cpp
  src/core/usecases/churn/churn_dataset.cpp
  src/core/pipeline/n8n_import.cpp
  src/core/pipeline/n8n_compile.cpp
  src/core/parquet/metadata.cpp
  src/core/parquet/snappy.cpp
  src/core/parquet/column_decoder.cpp
  src/core/parquet/reader.cpp
  src/core/parquet/writer.cpp
  src/core/input/python_frontend.cpp
  src/core/input/cluster_bindings.cpp
  src/core/input/datasets_bindings.cpp
  src/core/ir/execution_graph.cpp
  src/core/ir/serialization.cpp
  src/core/ddf/data_frame.cpp
  src/core/ddf/parquet_reader.cpp
  src/core/ddf/ops.cpp
  src/core/ddf/join.cpp
  src/core/ddf/data_frame_ops.cpp
  src/core/ddf/distributed_sort.cpp
  src/core/ddf/group_by.cpp
  src/core/ddf/as_of.cpp
  src/core/ddf/group_by_job.cpp
  src/core/ddf/local_partitions.cpp
  src/core/ddf/local_group_by_job.cpp
  src/core/ddf/featurize.cpp
  src/core/ddf/featurize_job.cpp
  src/core/ddf/local_featurize_job.cpp
  src/core/ml/gbdt.cpp
  src/core/ml/model_import.cpp
  src/core/ml/uplift.cpp
  src/core/ml/uplift_job.cpp
  src/core/ml/uplift_methodology.cpp
  src/core/ml/uplift_runner.cpp
  src/core/ml/next_best_action.cpp
  src/core/ml/next_best_action_execution.cpp
  src/core/ml/next_best_action_job.cpp
  src/core/ml/next_best_action_methodology.cpp
  src/core/ml/next_best_action_table.cpp
  src/core/ml/rank.cpp
  src/core/ml/rank_train_job.cpp
  src/core/ml/rank_runner.cpp
  src/core/ml/rank_methodology.cpp
  src/core/ml/language.cpp
  src/core/ml/language_train_job.cpp
  src/core/ml/language_runner.cpp
  src/core/ml/language_methodology.cpp
  src/core/ml/bandit.cpp
  src/core/ml/bandit_train_job.cpp
  src/core/ml/bandit_runner.cpp
  src/core/ml/streaming.cpp
  src/core/ml/backtest.cpp
  src/core/ml/forecast.cpp
  src/core/ml/anomaly.cpp
  src/core/ml/anomaly_job.cpp
  src/core/ml/anomaly_table.cpp
  src/core/ml/anomaly_plan.cpp
  src/core/ml/anomaly_methodology.cpp
  src/core/ml/forecast_runner.cpp
  src/core/ml/forecast_methodology.cpp
  src/core/ml/forecast_execution.cpp
  src/core/pipeline/forecast_graph_check.cpp
  src/core/ml/lag_features.cpp
  src/core/ml/temporal_split.cpp
  src/core/ml/gbdt_train_job.cpp
  src/core/ml/vision.cpp
  src/core/ml/vision_image.cpp
  src/core/ml/vision_runner.cpp
  src/core/ml/vision_train_job.cpp
  src/core/ml/vision_vit.cpp
  src/core/ml/calibration.cpp
  src/core/ml/tree_shap.cpp
  src/core/ml/gbdt_explain_job.cpp
  src/core/ml/gbdt_score.cpp
  src/core/ml/explain_row.cpp
  src/core/ml/score_table.cpp
  src/core/ml/segmentation.cpp
  src/core/ml/segmentation_job.cpp
  src/core/ml/segmentation_graph.cpp
  src/core/ml/segmentation_methodology.cpp
  src/core/ml/gbdt_score_job.cpp
  src/core/ml/local_train_job.cpp
  src/core/ml/local_featurize_train_job.cpp
  src/core/ml/golden_oracle.cpp
  src/core/ml/playbook.cpp
  src/core/agent/action.cpp
  src/core/agent/goal.cpp
  src/core/agent/conversation.cpp
  src/core/agent/conversation_walk.cpp
  src/core/agent/training_policy.cpp
  src/core/agent/agent_record.cpp
  src/core/agent/agent_store.cpp
  src/core/agent/base_prompt.cpp
  src/core/agent/interaction.cpp
  src/core/agent/response_facts.cpp
  src/core/agent/solution_graph.cpp
  src/core/dataset/auto_suggest_generator.cpp
  src/core/dataset/authored_generator.cpp
  src/core/dataset/base_agent_generator.cpp
  src/core/dataset/dataset_generator_factory.cpp
  src/core/dataset/interaction_families.cpp
  src/core/dataset/plan_templates.cpp
  src/core/dataset/product_claims.cpp
  src/core/agent/chat.cpp
  src/core/agent/model_cache.cpp
  src/core/agent/inference_runner.cpp
  src/core/agent/grader.cpp
  src/core/agent/sandbox.cpp
  src/core/agent/rollout.cpp
  src/core/agent/reward.cpp
  src/core/agent/admission_gate.cpp
  src/core/agent/curriculum.cpp
  src/core/agent/corpus_io.cpp
  src/core/agent/softmax_policy.cpp
  src/core/agent/advantage.cpp
  src/core/agent/policy_gradient.cpp
  src/core/agent/training.cpp
  src/core/agent/sequence_training.cpp
  src/core/agent/corpus_training.cpp
  src/core/agent/linear_policy.cpp
  src/core/agent/distributed.cpp
  src/core/agent/distributed_training.cpp
  src/core/agent/collectives_allreduce.cpp
  src/core/agent/neural_training.cpp
  src/core/agent/judge.cpp
  src/core/agent/qwen_rlvr.cpp
  src/core/agent/text_generation.cpp
  src/core/autograd/tensor.cpp
  src/core/autograd/transformer.cpp
  src/core/autograd/qwen.cpp
  src/core/autograd/safetensors.cpp
  src/core/tokenizer/byte_encoding.cpp
  src/core/tokenizer/pre_tokenizer.cpp
  src/core/tokenizer/bpe.cpp
  src/core/compute/cpu_device.cpp
  src/core/compute/device_registry.cpp
  src/core/compute/work_stealing_scheduler.cpp
  src/core/compute/distributed_scheduler.cpp
  src/core/compute/job_coordinator.cpp
  src/core/compute/coordinated_launch.cpp
  src/core/compute/checkpoint.cpp
  src/core/testkit/test_suite.cpp
  src/core/testkit/harness.cpp
  src/core/testkit/cases_compute.cpp
  src/core/testkit/cases_ddf.cpp
  src/core/testkit/cases_parquet.cpp
  src/core/testkit/cases_ml.cpp
  src/core/testkit/cases_distributed.cpp
  src/core/testkit/cases_connect.cpp
  src/core/testkit/cases_fulltext.cpp
  src/core/testkit/cases_governance.cpp
  src/core/testkit/cases_pipeline.cpp
  src/core/testkit/cases_uc1_scale.cpp
  src/core/testkit/cases_examples.cpp
  src/core/testkit/example_script.cpp
  src/core/testkit/async_case_run.cpp
  src/core/runtime.cpp
  src/core/repl/console_model.cpp
  src/core/repl/governed_console.cpp
  src/core/query/postgres.cpp
  src/core/query/statement_builders.cpp
  src/core/query/predicate.cpp
  src/core/query/distributed_select.cpp
  src/core/query/executor.cpp
  src/core/query/service.cpp
  src/core/headless.cpp
  src/core/signals.cpp
)

# Toolchain workaround: the pinned Homebrew Clang miscompiles the SQL parser's
# exception unwinding at -O3 + -fexperimental-library — a thrown ParseError
# escapes run()'s try/catch and std::terminate()s the process. It first bit the
# two-string CREATE TOKEN branch (isolated with a [[gnu::noinline]] helper + an
# out-of-TU statement builder; see src/core/query/statement_builders.cpp), but
# that mitigation is fragile: growing the grammar (the role-management DDL, #746)
# tips it over again TU-wide, aborting even pre-existing parse-error paths.
# Building just this one TU at -O1 makes its exception tables correct; it is a
# text parser over tiny inputs, so the optimization level is immaterial to
# performance. -O1 is a genuine sweet spot: BOTH -O3 (the target default) and -O0
# miscompile this TU's exception unwinding (a thrown ParseError escapes run()'s
# try/catch), so -O0 is NOT a safe "more conservative" fallback. The last -O flag
# wins, so this overrides the target's -O3.
#
# Two more constraints keep the miscompile at bay (both learned the hard way while
# adding CREATE VIRTUAL TABLE, #809):
#   1. Unwinding OUT OF a [[gnu::noinline]] parse helper is itself miscompiled at
#      -O1 (the throw is not caught), even though the helper is what keeps a big
#      grammar branch from bloating parse_create/run and tipping the TU-wide EH
#      over. So a helper that would throw must instead be NON-throwing — set a
#      pending-error member + return std::nullopt — and the caller does the actual
#      `throw` from its inline frame (inline throws ARE caught at -O1). See
#      parse_create_virtual_table_or_error.
#   2. The multi-string Statement *construction* miscompiles in-TU, so it is
#      routed out via statement_builders.cpp.
# Drop this whole dance when the compiler is bumped.
set_source_files_properties(src/core/query/postgres.cpp
  PROPERTIES COMPILE_OPTIONS "-O1")

target_compile_features(zetta-core PUBLIC cxx_std_26)

target_include_directories(zetta-core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

# PUBLIC: zetta::Node's interface exposes nlohmann::json (add_object) and the
# storage headers include it, so consumers (the GUI) get it transitively.
# Boost::capy backs the zetta::runtime async interface (include/zetta/runtime.hpp), also PUBLIC.
# zetta-compute houses the zetta::compute ComputeDevice backends (its public
# headers live in include/zetta/compute and are part of zetta-core's surface);
# PUBLIC so consumers linking zetta-core get the backend symbols + the optional
# ZETTA_WITH_CUDA define, while CUDA headers stay confined to that library.
target_link_libraries(zetta-core PUBLIC
  nlohmann_json::nlohmann_json Boost::capy zetta-compute)
# The reusable transport owns Corosio framing + its OpenSSL TLS provider.
# OpenSSL libcrypto remains here for certificate minting in
# src/core/cluster/security.cpp,
# automerge-cpp (the replication CRDT), and the JSON-Schema validator (boundary
# validation in src/core/pipeline/run.cpp) are used only by zetta-core's
# adapters — PRIVATE so they stay out of zetta-core's public surface and the GUI.
target_link_libraries(zetta-core PRIVATE
  zetta_transport
  OpenSSL::Crypto
  ZLIB::ZLIB
  automerge-cpp::automerge-cpp
  nlohmann_json_schema_validator)

# oramacore-cpp full-text engine — linked only when its submodule is present (see
# the guarded block above). PRIVATE: only src/core/index/oramacore_fulltext_index.cpp
# touches it, so its headers stay out of zetta-core's public surface (like the
# JSON-Schema validator). The PUBLIC define lets the backend's .cpp and consumers
# (tests) branch on engine availability (OramaCoreFulltextIndex::engine_available()).
if(ZETTA_WITH_ORAMACORE)
  target_link_libraries(zetta-core PRIVATE orama::oramacore-cpp)
  target_compile_definitions(zetta-core PUBLIC ZETTA_WITH_ORAMACORE=1)
endif()

# -----------------------------------------------------------------------------
# zetta-api — the embedded MCP server (docs/api/registry-design.md, #615).
# The registry/bridge/observability core lives in zetta-core (HTTP-free); this
# library holds the HTTP adapter TUs — the Beast2 server (inbound) and the
# native coroutine HTTPS client behind connect::HttpTransport (outbound, #827).
# beast2/http/Boost.JSON link PRIVATE so no HTTP type reaches consumers — the
# same isolation SDL/CUDA get. Corosio + its OpenSSL TLS provider are PRIVATE
# here too: the client dials sockets through zetta::runtime::net exactly as the
# cluster adapter does, and neither backend reaches zetta-core's surface.
# -----------------------------------------------------------------------------
add_library(zetta-api STATIC
  src/core/api/http_server.cpp
  src/core/api/native_http_transport.cpp
  src/core/api/clerk_auth.cpp)
target_compile_features(zetta-api PUBLIC cxx_std_26)
target_link_libraries(zetta-api PUBLIC zetta-core)
target_link_libraries(zetta-api PRIVATE
  Boost::beast2
  Boost::http
  Boost::json
  Boost::corosio
  Boost::corosio_openssl)

# The Postgres connector adapter (#892) — the one TU that names libpqxx, compiled
# into zetta-api only when the submodule is present, so zetta-core links no libpq.
if(ZETTA_WITH_POSTGRES)
  target_sources(zetta-api PRIVATE src/core/api/postgres_protocol.cpp)
  target_link_libraries(zetta-api PRIVATE pqxx)
  # PUBLIC so consumers (the executable's main.cpp, tests) can guard the install
  # call the same way — the CUDA/MLX pattern.
  target_compile_definitions(zetta-api PUBLIC ZETTA_WITH_POSTGRES=1)
endif()

# -----------------------------------------------------------------------------
# zetta (the executable: headless core + the SDL3 + Dear ImGui headed UI)
# -----------------------------------------------------------------------------

add_executable(zetta
  src/main.cpp
  src/gui/app.cpp
  src/gui/app_home.cpp
  src/gui/app_onboarding.cpp
  src/gui/app_views.cpp
  src/gui/app_palette.cpp
  src/gui/app_activity.cpp
  src/gui/app_explorer.cpp
  src/gui/app_graph_workspace.cpp
  src/gui/app_deploy.cpp
  src/gui/app_traces.cpp
  src/gui/app_runs.cpp
  src/gui/app_models.cpp
  src/gui/app_reports.cpp
  src/gui/app_agents.cpp
  src/gui/app_chat.cpp
  src/gui/app_datagen.cpp
  src/gui/app_datasets.cpp
  src/gui/app_search.cpp
  src/gui/app_jobs.cpp
  src/gui/app_cluster.cpp
  src/gui/app_cloud.cpp
  src/gui/app_governance.cpp
  src/gui/app_test_suite.cpp
  src/gui/repl_console.cpp
)

target_compile_features(zetta PRIVATE cxx_std_26)

target_include_directories(zetta PRIVATE
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(zetta PRIVATE
  zetta-core
  zetta-api
  imgui
  SDL3::SDL3-static
)

# Embed the Orama Studio typefaces — IBM Plex Sans (UI) + IBM Plex Sans
# SemiBold (headings) + IBM Plex Mono (labels / counts / metrics) — so the GUI
# can rasterize real TTFs at framebuffer-pixel density on HiDPI displays with
# no runtime font-file dependency, and the web app can serve the matching
# woff2 files from the node itself (GET /fonts/*, no external requests —
# air-gapped nodes have no egress). ImGui's default ProggyClean is a 13px
# bitmap font that goes blurry the moment SDL_SetRenderScale upscales it;
# AddFontFromMemoryTTF with RasterizerDensity = pixel_density keeps text
# crisp. The font files live in assets/fonts/ (SIL OFL 1.1; see
# assets/fonts/README.md).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
function(zetta_embed_font SRC_NAME HPP_NAME SYMBOL)
  add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/generated/${HPP_NAME}"
    COMMAND ${CMAKE_COMMAND}
            -DIN_FILE=${CMAKE_SOURCE_DIR}/assets/fonts/${SRC_NAME}
            -DOUT_FILE=${CMAKE_BINARY_DIR}/generated/${HPP_NAME}
            -DSYMBOL=${SYMBOL}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_font.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/fonts/${SRC_NAME}
            ${CMAKE_SOURCE_DIR}/cmake/embed_font.cmake
    COMMENT "Embedding ${SRC_NAME} into ${HPP_NAME}"
  )
endfunction()

# The GUI faces (TTF, rasterized by ImGui).
zetta_embed_font(IBMPlexSans-Regular.ttf  zetta_font_sans.hpp    kPlexSans)
zetta_embed_font(IBMPlexSans-SemiBold.ttf zetta_font_heading.hpp kPlexSansSemiBold)
zetta_embed_font(IBMPlexMono-Regular.ttf  zetta_font_mono.hpp    kPlexMono)
add_custom_target(zetta-font-embed ALL DEPENDS
  "${CMAKE_BINARY_DIR}/generated/zetta_font_sans.hpp"
  "${CMAKE_BINARY_DIR}/generated/zetta_font_heading.hpp"
  "${CMAKE_BINARY_DIR}/generated/zetta_font_mono.hpp")
add_dependencies(zetta zetta-font-embed)
target_include_directories(zetta PRIVATE "${CMAKE_BINARY_DIR}/generated")

# The web faces (woff2, served by the node at GET /fonts/* — latin subsets,
# ~15-25 KB each).
zetta_embed_font(IBMPlexSans-Regular.woff2  zetta_web_font_sans_400.hpp kPlexSansWoff400)
zetta_embed_font(IBMPlexSans-Medium.woff2   zetta_web_font_sans_500.hpp kPlexSansWoff500)
zetta_embed_font(IBMPlexSans-SemiBold.woff2 zetta_web_font_sans_600.hpp kPlexSansWoff600)
zetta_embed_font(IBMPlexMono-Regular.woff2  zetta_web_font_mono_400.hpp kPlexMonoWoff400)
add_custom_target(zetta-web-font-embed ALL DEPENDS
  "${CMAKE_BINARY_DIR}/generated/zetta_web_font_sans_400.hpp"
  "${CMAKE_BINARY_DIR}/generated/zetta_web_font_sans_500.hpp"
  "${CMAKE_BINARY_DIR}/generated/zetta_web_font_sans_600.hpp"
  "${CMAKE_BINARY_DIR}/generated/zetta_web_font_mono_400.hpp")
add_dependencies(zetta-api zetta-web-font-embed)
target_include_directories(zetta-api PRIVATE "${CMAKE_BINARY_DIR}/generated")

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
