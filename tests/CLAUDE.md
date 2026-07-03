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
- **No foreign runtime in build/CI** (ADR-17): goldens are committed fixtures,
  regenerated offline via env-gated write modes (`ZETTA_WRITE_GOLDEN=1`,
  `ZETTA_WRITE_SCHEMAS=1`; pyarrow fixtures via
  `scripts/gen-parquet-fixture.py`) — the generators are never build/CI
  dependencies. `tests/test_schemas.cpp` drift-checks `schemas/*.json` against
  the schema-as-code accessors.
- The **in-app test suite** (`zetta::testkit`, headless-runnable, rendered by
  the Operations → Test suite panel) is the catalog for engine-level canned
  cases — each new case gets a runbook under
  [docs/testing/runbooks/test-cases/](../docs/testing/runbooks/test-cases/README.md).
  See [docs/testing/](../docs/testing/README.md).
