# tests — conventions

GoogleTest suites, one file per area **under the domain's `tests/<domain>/`
directory** (mirroring `src/core/<domain>/`), linking **`zetta-core` only** — tests
exercise the headless core, never the GUI. Run:
`cmake --build build --target zetta-tests && ctest --test-dir build --output-on-failure`.

- **List every new test file explicitly in `tests/CMakeLists.txt`** — it is not
  globbed.
- **Add or extend tests whenever core logic changes** (storage, persistence,
  CRUD, parsing, cluster, replication, compute, ML). A boundary change gets a
  malformed-input case; a replicated entity gets a restart-reload case.
- `tests/temp_dir.hpp` gives each test an isolated, auto-cleaned `data_dir` —
  use it for anything that persists.
- **Multi-node tests run real Corosio TCP in-process** (loopback clusters, and
  `testkit::LoopbackCluster` for the in-app cases). They are timing-bound: make
  convergence loops **patient** — poll for seconds with a deadline, never a
  fixed small iteration count. Distributed ops assert
  **single-node == multi-node** equivalence.
- **Chaos experiments** live in `cluster/test_chaos.cpp` over
  `cluster/chaos_harness.hpp`, built as `zetta-chaos-tests` with the ctest
  label `chaos`. Only the unit cases run by default: the smoke skips unless
  `ZETTA_CHAOS_SMOKE=1`, the long runs unless
  `ZETTA_CHAOS_SCHEDULES` / `ZETTA_CHAOS_PROBE_ROUNDS` is set. A chaos step
  must prove its fault landed before its result counts — see
  [docs/testing/chaos-experiments.md](../docs/testing/chaos-experiments.md).
- **No foreign runtime in build/CI** (ADR-17): goldens are committed fixtures,
  regenerated offline via env-gated write modes (`ZETTA_WRITE_GOLDEN=1`,
  `ZETTA_WRITE_SCHEMAS=1`; pyarrow fixtures via
  `scripts/gen-parquet-fixture.py`) — the generators are never build/CI
  dependencies. `tests/test_schemas.cpp` drift-checks `schemas/*.json` against
  the schema-as-code accessors. `tests/test_release.cpp` holds the version
  together: `kVersion`, the CMake project version, the committed OpenAPI
  `info.version`, and `docs/releases/v<version>.md` listed in its index.
- The **in-app test suite** (`zetta::testkit`, headless-runnable, rendered by
  the Operations → Test suite panel) is the catalog for engine-level canned
  cases — each new case gets a runbook under
  [docs/testing/runbooks/test-cases/](../docs/testing/runbooks/test-cases/README.md).
  See [docs/testing/](../docs/testing/README.md).
- **A chat turn is testable without a checkpoint.** A test that only skips
  without `ZETTA_QWEN_DIR` proves nothing in a normal run (that is how a broken
  `agent_run` hid behind a green suite). Use the `ScriptedModel` in
  `tests/api/test_chat_loop.cpp`: a byte-level tokenizer on disk plus an
  installed inference runner that answers from a script and records every
  prompt, so the prompt, envelope parse, tool sandbox, grounding and proposal
  all run for real. Restore the previous runner when done.
