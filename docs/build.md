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
- macOS for the headed UI (SDL3 uses a Metal-backed renderer); the headless core
  is portable.

SDL3, Dear ImGui (+ `imgui_stdlib`), and GoogleTest are fetched via CMake
`FetchContent` — no `brew install` needed for the stack. nlohmann/json comes from
automerge-cpp's vendored copy (a submodule), not FetchContent. ccache is used as
the compiler launcher when present.

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

## Run

```bash
./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta --listen 127.0.0.1:7777 # restrict to loopback (default is 0.0.0.0:7777, all interfaces)
./build/bin/zetta --port-range 16       # ports to try from --listen, for several nodes/host
./build/bin/zetta --peer HOST:PORT      # join another node (repeatable; all peers equal)
./build/bin/zetta --help
```

On startup a node prints its id, the address(es) it's reachable at, and the
command to add another node — see [cluster.md](cluster.md).

In headed mode, the **Query (postgres)** panel runs SQL against the selected
database; the **Databases | Tables | Rows** explorer reflects changes live.

## Test

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

Tests (GoogleTest) live in `tests/`, link `zetta-core`, and cover the headless
core only. Disable building them with `-DZETTA_BUILD_TESTS=OFF`.
