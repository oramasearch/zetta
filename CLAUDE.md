# zetta — Claude rules

A C++23 desktop app built on **SDL3 + Dear ImGui**. macOS-first. Single
statically-linked binary — no .dylib next to the executable, no brew install
ritual for the GUI stack (everything comes via `FetchContent`).

## Tech stack

- **C++23** (Apple Clang ships `std::expected` etc. under `-std=c++2b`)
- **CMake 3.28+ / Ninja**, all deps via `FetchContent`:
  - **SDL3** (release-3.2.0) — window + input + Metal-backed renderer
  - **Dear ImGui** (v1.91.5) — immediate-mode GUI, SDL3 + SDL_Renderer backend
- **ccache** used as the compiler launcher when present (auto-detected).

## Build + run

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/bin/zetta
```

The first configure clones SDL3 + ImGui into `build/_deps/` (a few minutes);
subsequent builds are fast. `build/` is git-ignored.

## Code layout

```
CMakeLists.txt                ← the whole build graph (SDL3 + ImGui + zetta)
cmake/embed_font.cmake        ← bakes a .ttf into a C++ header at configure time
src/main.cpp                  ← SDL3 + ImGui main loop + HiDPI handling
build/generated/              ← embedded font header (generated; git-ignored)
```

As the app grows, the worktree-manager pattern this was copied from splits the
UI into `src/gui/views/*.cpp` renderers dispatched from `main.cpp` over an
`AppState`/`Phase` state machine. Reach for that shape once `main.cpp` has more
than one screen.

## The two non-obvious things in main.cpp

These are the parts worth understanding before you touch the render path —
they're load-bearing and easy to break:

1. **Per-monitor scaling (`apply_display_scale`).** SDL3 separates *pixel
   density* (Retina; the ImGui SDLRenderer backend already handles this) from
   *content scale* (the OS "make the UI bigger" hint; ImGui does NOT apply it).
   `SDL_GetWindowDisplayScale` multiplies the two together — using it directly
   double-scales the UI on Retina. We read `SDL_GetDisplayContentScale` for the
   content factor only, and re-rasterize the font atlas at framebuffer-pixel
   density (`RasterizerDensity`) so text stays crisp when the display changes.

2. **The `SDL_SetRenderScale` call before clearing.** Because the window is
   created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`, the framebuffer is larger than
   the logical window. The SDLRenderer3 backend does NOT set the render scale
   for us, so without this line the UI paints into the top-left quadrant only.

The font is embedded (DroidSans.ttf from the ImGui tree) via
`cmake/embed_font.cmake` so the binary has no runtime asset dependency — the
same rasterize-at-density trick keeps it sharp on HiDPI.

## Conventions

### Commit messages
- **Conventional Commits** (`feat:`, `fix:`, `refactor:`, `docs:`, etc.).
- Imperative mood; subject ≤72 chars; body explains *why*.
- **Never mention "Claude" / "AI" / "Codex"** in commit text or PR bodies.
- **Never add Claude as an author or co-author.** No `Co-Authored-By: Claude`
  trailer, no "Generated with Claude Code" footer — on commits or PRs. The
  commit author is the user, full stop.

### Idle loop
The main loop uses `SDL_WaitEventTimeout` (≈30fps idle cap) rather than a busy
vsync spin — it wakes immediately on input so interaction stays full-speed but
doesn't burn CPU redrawing identical frames. Keep any background polling cheap
enough to run inside that cadence.
