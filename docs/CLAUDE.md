# docs — filing rules

This tree is **domain-organized for progressive disclosure**:
[README.md](README.md) (the domain table) → `docs/<domain>/README.md` (overview
+ one-line page index) → the page. Docs land **in the same commit** as the
change they describe — never as a follow-up.

- **One page per shipped slice**, filed under its domain. If a slice spans
  domains, file it where its engine code lives and cross-link from the others.
- **Adding a page = updating its domain `README.md` index** (one line, ~120
  chars, pedagogical order — foundations first) in the same commit. Add to the
  [README.md](README.md) domain table only when creating a whole new domain.
- **[architecture/code-map.md](architecture/code-map.md)** is the annotated
  file-by-file map — update it whenever source files are added, moved, or
  renamed.
- Long-form background (not descriptions of shipped code) goes in
  [research/](research/) with a provenance header (source + date + status);
  forward-looking work plans go in [plans/](plans/README.md).
- Cross-domain links are **relative** (`../ddf/ddf.md`); cross-repo references
  are **full URLs** (never bare paths into another repo).
- Pages state their **boundaries** (which JSON/wire formats they add — ideally
  "no new JSON format"), their issue refs (`#NNN`), and what is deliberately a
  **later slice**.
- **[full-context.md](full-context.md) is the reload map** — when your
  change shifts project state (a PR lands, a plan reshapes, a foundation or
  wave ships), refresh the map's Current state section in the same commit.
- Before committing, **grep this tree for stale terms** — renamed symbols,
  removed flags, moved files. No doc may mention the old name.
