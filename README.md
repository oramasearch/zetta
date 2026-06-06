# zetta

A C++23 desktop app built on **SDL3 + Dear ImGui** — single statically-linked
binary, macOS-first.

## Build + run

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/bin/zetta
```

The first configure clones SDL3 and Dear ImGui via CMake `FetchContent` (a few
minutes); subsequent builds are fast (ccache-backed when available). No
`brew install` step is needed for the GUI stack.

## Requirements

- CMake 3.28+ and Ninja
- A C++23 compiler (Apple Clang 15+ / recent Clang or GCC)
- macOS (SDL3 uses a Metal-backed renderer); other platforms should work but
  are untested
