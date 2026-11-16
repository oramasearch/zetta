# Embedded fonts

The Orama Studio typefaces — **IBM Plex Sans + IBM Plex Mono** (the Studio
design system, the design repository `packages/design-system/`). Every
file here is **baked into a binary** at build time by
`cmake/embed_font.cmake` (→ `build/generated/zetta_font_*.hpp` /
`zetta_web_font_*.hpp`), so neither surface has a runtime font-file
dependency and the web app never makes an external font request.

**Desktop (TTF → the `zetta` binary)** — `src/gui/app.cpp` rasterizes these
with ImGui at framebuffer-pixel density:

| File | Use |
|------|-----|
| `IBMPlexSans-Regular.ttf` | UI sans (body, buttons, nav) |
| `IBMPlexSans-SemiBold.ttf` | headings (`page_header`, the sidebar lockup) |
| `IBMPlexMono-Regular.ttf` | mono labels / counts / metrics / paths |

**Web (woff2 latin subsets → `zetta-api`)** — served by the node itself at
`GET /fonts/*.woff2` (immutable-cacheable), consumed by the `@font-face`
rules in `web_ui_stylesheet()`:

| File | Route |
|------|-------|
| `IBMPlexSans-Regular.woff2` | `/fonts/plex-sans-400.woff2` |
| `IBMPlexSans-Medium.woff2` | `/fonts/plex-sans-500.woff2` |
| `IBMPlexSans-SemiBold.woff2` | `/fonts/plex-sans-600.woff2` |
| `IBMPlexMono-Regular.woff2` | `/fonts/plex-mono-400.woff2` |

Sources: the IBM Plex project (<https://github.com/IBM/plex>, static TTFs
from the `@ibm/plex-sans` / `@ibm/plex-mono` release zips) and the Fontsource
woff2 latin subsets. All licensed **SIL OFL 1.1** — see `OFL.txt`.

`Geist.ttf` / `GeistMono.ttf` (also OFL, <https://github.com/vercel/geist-font>)
remain in tree as the pre-Studio faces; nothing loads them.
