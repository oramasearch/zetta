# AGENTS.md

Read `CLAUDE.md` first. It is the instruction file for this repository and applies
in full to every coding agent. Then `README.md` and `docs/README.md`.

Non-negotiables, restated from `CLAUDE.md`:

- Every change documents itself (`docs/<topic>/*.md`, or the `README.md` where the repo has no
  `docs/`) **in the same commit**, and updates status lines when they stop being true.
- **No AI attribution anywhere.** No `Co-Authored-By`, no "Generated with", no session or model
  trailers, in commits or pull requests.
- No time estimates. The word "seam" is banned.
- Work on a feature branch named for the issue, open a pull request against `dev` that references the
  issue, and never push to `main` or `dev` directly.
- C++26; build and test with CMake (`cmake -B build -G Ninja && cmake --build build && ./build/tests/zetta-tests`). ADR-17: no embedded Python, no foreign ML runtime, no hosted model API; methodologies are native and distributed the way GBDT is.
- Regenerate `schemas/openapi.json` with `ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests` when a capability changes.
