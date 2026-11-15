# The Studio web app bundle (vendored build)

**Not in the public export.** `.public-exclude` leaves this folder out until the bundle is rebuilt
from a cleaned source (https://github.com/oramasearch/zetta/issues/1526); nothing in the build,
the install rules or the tests reads it.

The design repo's React Studio app, built and vendored here so the node (and
the headless image) can serve it at `/studio/` with no npm anywhere in
zetta's build. The engine serves it when `--webapp-dir` (or the image-seated
`ZETTA_WEBAPP_DIR`) points here; its only data path is the `/v1` surface.

- Source: the design repository @ `fc2cc41` (`npx vite build`)
- Refresh: rebuild in the design repo, copy `dist/`'s app files here
  (index.html · assets/ · fonts/ · favicon + brand files), commit with the
  new source sha. Storybook / gamma / archives are deliberately excluded.
- Licensing: app code MIT-side (the design repo); fonts OFL (IBM Plex, Geist).
  The design system's vendored Motion+ components are NOT part of this build's
  shipped chunks unless licensed — verify on refresh.
