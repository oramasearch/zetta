# Build, run, test

## Prerequisites

Install the system build tools with the helper — macOS (Homebrew) or
Ubuntu/Debian (apt):

```bash
./scripts/install-deps.sh
```

What it installs (or install manually):

- CMake 3.28+ and Ninja
- A **C++26 compiler — Clang 17+** (or GCC 14+). zetta is C++26 and the
  Capy/Corosio stack needs Clang 17+, so older system defaults (Apple Clang 15,
  Ubuntu's GCC 13) won't build it. CMake auto-selects one in a pre-`project()`
  block: Homebrew LLVM (`/opt/homebrew/opt/llvm`, + SDK sysroot + its libc++) on
  macOS; the newest installed `clang++-NN` (≥17) on Linux. Override with
  `-DCMAKE_CXX_COMPILER=...` (e.g. `clang++-19` or `g++-14`) or
  `-DZETTA_USE_SYSTEM_COMPILER=ON`. `install-deps.sh` installs a suitable clang.
  With any Homebrew LLVM (the `llvm` keg or a versioned `llvm@NN`) the build
  links Homebrew's own libc++ and defines `_LIBCPP_DISABLE_AVAILABILITY`:
  since LLVM 21.1.8 the headers availability-gate symbols the macOS *system*
  dylib lacks (floating-point `std::from_chars` → "'from_chars' is
  unavailable: introduced in macOS 26.0"), which is a false alarm when running
  against Homebrew's dylib.
- **OpenSSL 3** (`openssl@3` on Homebrew, `libssl-dev` on apt) — **required**:
  it backs the cluster's inter-node mutual TLS (Corosio's TLS provider) and the
  node-identity certificate minting (CTL-2, #466;
  [docs/cluster/security.md](cluster/security.md)). CMake fails the configure
  with a clear message when it's missing.
- macOS for the headed UI (SDL3 uses a Metal-backed renderer); the headless core
  is portable.

SDL3, Dear ImGui (+ `imgui_stdlib`), GoogleTest, and the
[json-schema-validator](https://github.com/pboettch/json-schema-validator)
(pboettch, for the Run record's storage-boundary validation) are fetched via CMake
`FetchContent` — no `brew install` needed for the stack. nlohmann/json comes from
automerge-cpp's vendored copy (a submodule), not FetchContent; the validator
reuses that same nlohmann/json target. ccache is used as the compiler launcher
when present.

## Submodules

The coroutine stack (Capy, Corosio) and the replication library (automerge-cpp,
which itself vendors nlohmann/json as a nested submodule) are vendored under
`modules/` as git submodules. Clone with them — `--recursive` matters because
automerge-cpp nests submodules — or the build fails at
`add_subdirectory(modules/capy)` / a missing `upstream/json`:

```bash
git clone --recurse-submodules git@github.com:homer6/zetta.git
# existing checkout:
git submodule update --init --recursive
```

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The first configure clones the deps into `build/_deps/` (a few minutes);
later builds are fast. `build/` is git-ignored.

### Optional CUDA compute backend

The accelerated-compute backends build into a `zetta-compute` static library that
`zetta-core` links. The **default build is CPU-only** and needs no GPU toolkit
(configure logs `CUDA compute backend: disabled (CPU-only build)`). To build the
optional CUDA backend (`zetta::compute::CudaDevice`) on a host with the CUDA
toolkit:

```bash
cmake -S . -B build -G Ninja -DZETTA_ENABLE_CUDA=ON
```

This requires `find_package(CUDAToolkit)` to succeed (configure fails loudly
otherwise) and compiles `src/core/compute/cuda_device.cu` with nvcc. The CUDA
headers stay confined to that library — they never reach `zetta-core` — mirroring
how SDL/ImGui are kept out. See [compute-device.md](compute/compute-device.md) for the
backend and its verification caveats.

## Run

```bash
./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta --listen 127.0.0.1:7777 # restrict to loopback (default is 0.0.0.0:7777, all interfaces)
./build/bin/zetta --port-range 16       # ports to try from --listen, for several nodes/host
./build/bin/zetta --peer HOST:PORT      # join another node (repeatable; all peers equal; HOST:PORT,HOST:PORT lists one peer's candidate endpoints)
./build/bin/zetta --advertise HOST:PORT # extra reachable endpoint(s) to advertise (a NAT'd/LB'd node's public address; repeatable)
./build/bin/zetta --log-cluster         # log gossip events to stderr (debugging)
./build/bin/zetta --no-preflight        # skip the startup integrity check
./build/bin/zetta --help
```

On startup a node runs a **preflight integrity check** by default (disable with
`--no-preflight`): it re-hashes each local large-object body against its recorded
SHA-256 and looks for dangling references, logging a `preflight:` summary. See
[storage.md](data/storage.md).

On startup a node prints its id, the address(es) it's reachable at, and the
command to add another node — see [cluster.md](cluster/cluster.md).

In headed mode, the **Query (postgres)** panel runs SQL against the selected
database; the **Correctness gate** panel validates an execution graph and shows
the per-check report + block-on-fail state (see [gate.md](pipeline/gate.md)); the
**Projects | Databases | Tables | Rows | Large Objects** explorer reflects
changes live (the leftmost **Projects** column creates/selects/renames AI
projects — see [projects.md](pipeline/projects.md)).

## Test

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

Tests (GoogleTest) live in `tests/`, link `zetta-core`, and cover the headless
core only. Disable building them with `-DZETTA_BUILD_TESTS=OFF`.

## Container builds (`.dockerignore`)

The image Dockerfiles live outside this repo — `build/images/zetta-headless/` and
`build/images/zetta-e2e/` in `zetta-bridge` — but they use **this repository root as the
build context** and do `COPY . /src`, so the context is filtered by the `.dockerignore`
here.

It exists for a specific failure. Without it, a host `build/` directory is copied into
the image, and its `CMakeCache.txt` records absolute *host* paths, so the in-image
`cmake -S . -B build` aborts before compiling anything:

```
CMake Error: The current CMakeCache.txt directory /src/build/CMakeCache.txt is different
than the directory /home/you/zetta/build where CMakeCache.txt was created.
```

Any machine that has ever run a local build hits this. The file also drops the context
from ~1.1 GB to ~55 MB.

Two things to know before editing it:

- **Patterns are relative to the context root**, so a bare name matches only at the top
  level. `/build/` excludes this repo's build output and nothing else.
- **Never exclude `**/build`.** The vendored Boost-style submodules (`capy`, `corosio`,
  `http`, `beast2`) each track a real `build/` directory of Jamfiles — that is source.

Verify a change to it by measuring the context, not by reading the patterns:

```bash
printf 'FROM busybox\n' > /tmp/ctx.Dockerfile
DOCKER_BUILDKIT=0 docker build -f /tmp/ctx.Dockerfile . 2>&1 | grep 'build context'
```
