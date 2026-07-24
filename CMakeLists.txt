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

# When built by Homebrew clang (the default `llvm` keg or a versioned
# `llvm@NN`), run against its newer libc++ (its headers are ahead of the
# system dylib); link + rpath the matching runtime. Because we run against
# Homebrew's own libc++, its headers' availability annotations — which gate
# symbols on the macOS *system* dylib, e.g. floating-point std::from_chars
# ("introduced in macOS 26.0" since LLVM 21.1.8) — are false alarms here:
# disable them.
if(APPLE AND CMAKE_CXX_COMPILER MATCHES "^(/opt/homebrew/opt/llvm[^/]*)/")
  set(ZETTA_HOMEBREW_LLVM_PREFIX "${CMAKE_MATCH_1}")
  add_link_options(-L${ZETTA_HOMEBREW_LLVM_PREFIX}/lib/c++
                   -Wl,-rpath,${ZETTA_HOMEBREW_LLVM_PREFIX}/lib/c++)
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

# Corosio — coroutine-native networking (needs Capy above). Its OpenSSL TLS
# provider backs the cluster's inter-node mutual TLS (CTL-2, #466), so OpenSSL
# is REQUIRED (scripts/install-deps.sh installs it; keg-only on Homebrew).
# Found here at the top level so both Corosio's provider and zetta-core's
# certificate minting (src/core/cluster/security.cpp) see the same targets.
if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)
set(BOOST_COROSIO_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BOOST_COROSIO_BUILD_PERF     OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/corosio EXCLUDE_FROM_ALL)

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
  list(APPEND ZETTA_COMPUTE_SOURCES src/core/compute/cuda_device.cu)
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
  src/core/observability/activity.cpp
  src/core/observability/trace.cpp
  src/core/observability/crash.cpp
  src/core/observability/feedback.cpp
  src/core/api/background.cpp
  src/core/api/bridge.cpp
  src/core/api/capabilities.cpp
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
  src/core/cloud/provisioner.cpp
  src/core/cloud/provisioner_detect.cpp
  src/core/cloud/add_nodes.cpp
  src/core/cloud/local_provisioner.cpp
  src/core/cloud/provisioner_fleet.cpp
  src/core/cloud/command_runner.cpp
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
  src/core/ml/model.cpp
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
  src/core/governance/token_secret.cpp
  src/core/governance/user.cpp
  src/core/governance/role.cpp
  src/core/governance/token.cpp
  src/core/governance/user_role.cpp
  src/core/governance/policy.cpp
  src/core/data/table.cpp
  src/core/data/partitioner.cpp
  src/core/data/partition_body.cpp
  src/core/storage/lazy_row.cpp
  src/core/storage/jsonl_store.cpp
  src/core/storage/parquet_store.cpp
  src/core/util/uuid.cpp
  src/core/util/sha256.cpp
  src/core/util/timestamp.cpp
  src/core/pipeline/validation.cpp
  src/core/pipeline/gate.cpp
  src/core/pipeline/graph_checks.cpp
  src/core/usecases/churn/entity_resolution_check.cpp
  src/core/usecases/churn/entity_resolution.cpp
  src/core/pipeline/executor.cpp
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
  src/core/data/add_dataset.cpp
  src/core/connect/connection_store.cpp
  src/core/connect/connector.cpp
  src/core/connect/distributed_fetch.cpp
  src/core/connect/distributed_ingest.cpp
  src/core/connect/snowflake_protocol.cpp
  src/core/connect/http_transport.cpp
  src/core/connect/model_fetch.cpp
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
  src/core/ml/gbdt_train_job.cpp
  src/core/ml/calibration.cpp
  src/core/ml/tree_shap.cpp
  src/core/ml/gbdt_explain_job.cpp
  src/core/ml/gbdt_score.cpp
  src/core/ml/gbdt_score_job.cpp
  src/core/ml/local_train_job.cpp
  src/core/ml/local_featurize_train_job.cpp
  src/core/ml/golden_oracle.cpp
  src/core/ml/playbook.cpp
  src/core/agent/action.cpp
  src/core/agent/agent_record.cpp
  src/core/agent/agent_store.cpp
  src/core/agent/base_prompt.cpp
  src/core/agent/solution_graph.cpp
  src/core/dataset/auto_suggest_generator.cpp
  src/core/dataset/base_agent_generator.cpp
  src/core/dataset/dataset_generator_factory.cpp
  src/core/dataset/plan_templates.cpp
  src/core/agent/chat.cpp
  src/core/agent/model_cache.cpp
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
# Boost::capy backs the zetta::runtime async seam (include/zetta/runtime.hpp), also PUBLIC.
# zetta-compute houses the zetta::compute ComputeDevice backends (its public
# headers live in include/zetta/compute and are part of zetta-core's surface);
# PUBLIC so consumers linking zetta-core get the backend symbols + the optional
# ZETTA_WITH_CUDA define, while CUDA headers stay confined to that library.
target_link_libraries(zetta-core PUBLIC
  nlohmann_json::nlohmann_json Boost::capy zetta-compute)
# Corosio (networking) + its OpenSSL TLS provider (inter-node mTLS, CTL-2),
# OpenSSL libcrypto (certificate minting in src/core/cluster/security.cpp),
# automerge-cpp (the replication CRDT), and the JSON-Schema validator (boundary
# validation in src/core/pipeline/run.cpp) are used only by zetta-core's
# adapters — PRIVATE so they stay out of zetta-core's public surface and the GUI.
target_link_libraries(zetta-core PRIVATE
  Boost::corosio
  Boost::corosio_openssl
  OpenSSL::Crypto
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
  src/core/api/native_http_transport.cpp)
target_compile_features(zetta-api PUBLIC cxx_std_26)
target_link_libraries(zetta-api PUBLIC zetta-core)
target_link_libraries(zetta-api PRIVATE
  Boost::beast2
  Boost::http
  Boost::json
  Boost::corosio
  Boost::corosio_openssl)

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
