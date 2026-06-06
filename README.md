# zetta

A database. A *zetta node* is one running instance; it runs **headed** (default
— with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers with no
display). C++23, macOS-first, single statically-linked binary.

## Build + run

```bash
cmake -S . -B build -G Ninja
cmake --build build

./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta --help
```

The first configure clones SDL3 and Dear ImGui via CMake `FetchContent` (a few
minutes); subsequent builds are fast (ccache-backed when available). No
`brew install` step is needed for the GUI stack.

## Requirements

- CMake 3.28+ and Ninja
- A C++23 compiler (Apple Clang 15+ / recent Clang or GCC)
- macOS (SDL3 uses a Metal-backed renderer); other platforms should work but
  are untested
