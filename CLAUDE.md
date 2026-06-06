# zetta — Claude rules

**zetta is a database.** A *zetta node* is one running instance of it. A node
can run in two modes:

- **headed** (default) — the node plus an SDL3 + Dear ImGui UI rendering it.
- **headless** — the node alone, no UI, for servers with no display.

C++23, macOS-first. Single statically-linked binary; the whole GUI stack comes
via CMake `FetchContent`, so there's no `brew install` ritual.

## Architecture

The core rule: **`zetta::Node` knows nothing about any UI.** It owns the
database lifecycle and is the only thing headless mode needs. The GUI is a thin
renderer driven by a Node; headless mode drives the same Node with no renderer.
This is enforced at the build level — the `zetta-core` static library has **no
SDL/ImGui dependency** — which is what guarantees a node can run on a headless
server.

```
main()                       picks the mode, owns the Node lifecycle
  │  builds zetta::Node, node.start()
  ├── --headless ─→ zetta::run_headless(node)     # core only, no SDL
  └── --headed   ─→ zetta::gui::App(node).run()   # SDL3 + ImGui over the node
  ...then node.stop()
```

The storage + query engine doesn't exist yet. Today `Node` is a lifecycle
skeleton (`start` / `tick` / `stop` + a heartbeat) so both run modes have a real
object to drive. **Build the engine out behind the `Node` interface** — don't
let SDL/ImGui types leak into `zetta-core`.

## Tech stack

- **C++23** (Apple Clang ships `std::expected` etc. under `-std=c++2b`)
- **CMake 3.28+ / Ninja**, GUI deps via `FetchContent`:
  - **SDL3** (release-3.2.0) — window + input + Metal-backed renderer
  - **Dear ImGui** (v1.91.5) — immediate-mode GUI, SDL3 + SDL_Renderer backend
- **ccache** used as the compiler launcher when present (auto-detected).

## Build + run

```bash
cmake -S . -B build -G Ninja
cmake --build build

./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta --help
```

The first configure clones SDL3 + ImGui into `build/_deps/` (a few minutes);
subsequent builds are fast (ccache). `build/` is git-ignored.

## Code layout

```
CMakeLists.txt              build graph: imgui lib, zetta-core lib, zetta exe
cmake/embed_font.cmake      bakes a .ttf into a C++ header at configure time
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the database node
  headless.hpp              zetta::run_headless(Node&)
src/core/                   zetta-core impl
  node.cpp
  headless.cpp              signal-driven headless loop
src/gui/                    headed UI (links SDL3 + ImGui)
  app.hpp / app.cpp         zetta::gui::App — window + render loop over a Node
src/main.cpp                arg parsing + mode dispatch
build/generated/            embedded font header (generated; git-ignored)
```

`src/gui/app.hpp` forward-declares SDL/ImGui types so only `app.cpp` pulls in
those heavy headers — keep it that way.

## The two non-obvious things in `src/gui/app.cpp`

These are load-bearing and easy to break:

1. **Per-monitor scaling (`App::apply_display_scale`).** SDL3 separates *pixel
   density* (Retina; the ImGui SDLRenderer backend already handles this) from
   *content scale* (the OS "make the UI bigger" hint; ImGui does NOT apply it).
   `SDL_GetWindowDisplayScale` multiplies the two — using it directly
   double-scales on Retina. We read `SDL_GetDisplayContentScale` for the content
   factor only, and re-rasterize the font atlas at framebuffer-pixel density so
   text stays crisp when the display changes.

2. **The `SDL_SetRenderScale` call before clearing.** The window uses
   `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so the framebuffer is larger than the
   logical window. The SDLRenderer3 backend does NOT set the render scale for
   us — without this line the UI paints into the top-left quadrant only.

The font (DroidSans.ttf from the ImGui tree) is embedded via
`cmake/embed_font.cmake` into `namespace zetta`, so the binary has no runtime
asset dependency.

## Known gaps / behavior notes

- **Headed mode doesn't quit on terminal Ctrl-C** — SDL3 on macOS doesn't turn
  SIGINT into a quit event here, so close the window or use Cmd+Q. Only headless
  mode installs SIGINT/SIGTERM handlers. (Wiring a handler into headed mode is a
  reasonable future tweak if it becomes annoying.)

## Conventions

### Commit messages
- **Conventional Commits** (`feat:`, `fix:`, `refactor:`, `docs:`, etc.).
- Imperative mood; subject ≤72 chars; body explains *why*.
- **Never mention "Claude" / "AI" / "Codex"** in commit text or PR bodies.
- **Never add Claude as an author or co-author.** No `Co-Authored-By: Claude`
  trailer, no "Generated with Claude Code" footer — on commits or PRs. The
  commit author is the user, full stop.

### Committing + pushing
- **Always commit and push after any major work.** When a meaningful unit of
  work is done and verified, `git commit` it and `git push` to `origin/main` —
  don't leave finished work uncommitted or unpushed. Push directly to `main`
  (this is a personal repo; no PR ceremony).

### Idle loop
The headed loop uses `SDL_WaitEventTimeout` (≈30fps idle cap) rather than a busy
vsync spin — it wakes immediately on input so interaction stays full-speed but
doesn't burn CPU on identical frames. The headless loop ticks at 10 Hz. Keep any
background work cheap enough to run inside those cadences (or move it off the
loop thread).
