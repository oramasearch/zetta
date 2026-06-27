# Embedded fonts

The headed GUI's typeface, per the Orama design system (Geist + Geist Mono).
These TTFs are **baked into the binary** at build time by `cmake/embed_font.cmake`
(→ `build/generated/zetta_font_sans.hpp` + `zetta_font_mono.hpp`), so the runtime
has no font-file dependency. `src/gui/app.cpp` loads **Geist** as the UI font and
**Geist Mono** as the secondary face for labels, counts, metrics, and code-ish
text (paths, commands, SQL).

| File | Use |
|------|-----|
| `Geist.ttf` | UI sans (body, headings, buttons, nav) |
| `GeistMono.ttf` | mono labels / counts / metrics / paths |
| `OFL.txt` | the SIL Open Font License both ship under |

Both are the upstream **variable** fonts (weight axis); ImGui's stb_truetype
rasterizes the default instance (Regular). Source: the Geist Project
(<https://github.com/vercel/geist-font>), via the Google Fonts mirror. Licensed
**SIL OFL 1.1** — see `OFL.txt`.
