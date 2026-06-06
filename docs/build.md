# Build, run, test

## Prerequisites

- CMake 3.28+ and Ninja
- A C++23 compiler (Apple Clang 15+ / recent Clang or GCC)
- macOS for the headed UI (SDL3 uses a Metal-backed renderer); the headless core
  is portable.

SDL3, Dear ImGui (+ `imgui_stdlib`), nlohmann/json, and GoogleTest are fetched
via CMake `FetchContent` — no `brew install` needed for the stack. ccache is
used as the compiler launcher when present.

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
./build/bin/zetta --help
```

In headed mode, the **Query (postgres)** panel runs SQL against the selected
database; the **Databases | Tables | Rows** explorer reflects changes live.

## Test

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

Tests (GoogleTest) live in `tests/`, link `zetta-core`, and cover the headless
core only. Disable building them with `-DZETTA_BUILD_TESTS=OFF`.
