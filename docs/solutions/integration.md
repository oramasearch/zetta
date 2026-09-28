# Landing S-1 through S-4 and S-12

Refs #1122 and #1128. The integration branch combines S-1 (#1135), S-2
(#1141), S-3 (#1140), S-4 (#1139), and S-12 (#1142), followed by `dev`.
Module branches retain merge commits and their original tests. This landing
does not complete the catalogue epic: S-5 through S-11 and the live lake
acceptance remain outside this work order.

## Shared code resolutions

- The correctness gate keeps all 51 checks. After S-1, module checks register
  in S-2, S-3, S-4, S-12 order. Both ordered-name tests assert the full list.
- M-1 keeps lead conversion, continuous-value regression, collections recovery,
  and claim referral shapes. The shared executor honors each shape's result
  database, table prefix, entity column, and score column; squared-error
  models retain the lifetime-value defaults.
- Foundation binding retains every module's correctness profile, decision
  source, policy, holdout, action payload, and monitor. S-1, S-3, and S-4 keep
  M-6 treatment output; S-12 also carries M-11 model metrics into monitoring.
- All module journeys and solution tests remain present. Documentation keeps
  each module's data, policy, refusal, and measurement contract.
- The `dev` merge retains pub/sub CRUD, incremental solution reconciliation,
  distributed entity resolution, API changes, and their tests. Generated
  schemas are rebuilt from the combined capability registry.
- The final `dev` refresh also retains #1100's asynchronous connection
  admission, explicit overload response, IO-executor runtime helpers, and
  long-poll regression tests. Its public-header changes require both native
  build trees to be discarded and configured afresh before the final proof.

No new JSON or wire format is introduced by the integration itself. Caller
snapshot cutoffs, delivery credentials, and ROI economics remain required.

## Verification

The counts below record the proof before the final #1100 refresh. A fresh
static/shared proof is required for the refreshed tree before it is pushed.

Native static and shared-dependency suites are run separately with at most
six build jobs. The complete `zetta-tests` binary supplies the proof counts;
the only pre-authorized timing waivers are #959 and #1124. OpenAPI and boundary
schemas are regenerated with `ZETTA_WRITE_SCHEMAS=1`, then drift-checked.

The native proof uses a fresh local PostgreSQL database, with the search
engine enabled and no CUDA. Build caches and test temporary files stay in the
worktree. The proof environment was:

```sh
export CCACHE_DIR="$PWD/build/ccache"
export CCACHE_TEMPDIR="$PWD/build/ccache/tmp"
export TMPDIR="$PWD/build/test-tmp"
export ZETTA_PG_TEST_HOST=127.0.0.1
export ZETTA_PG_TEST_USER=stevesperandeo
export ZETTA_PG_TEST_DB=zetta_issue1122_20260928_proof_8127
```

Configure and build commands:

```sh
cmake -B build -G Ninja -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake --build build -j 6
cmake -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build build-shared --target boost_beast2 zetta-tests -j 6
cmake --build build-shared --target zetta-refused-start-test -j 6
```

Schema regeneration and focused proof:

```sh
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests \
  --gtest_filter='Schemas.*:OpenApi.*:SolutionRegistry.*:SolutionJourney.*:CorrectnessGate.*:GraphChecksThroughGate.*' \
  --gtest_output=xml:build/focused.xml
```

```text
[==========] 43 tests from 6 test suites ran. (45504 ms total)
[  PASSED  ] 43 tests.
```

Full proofs, executed one suite at a time:

```sh
./build/tests/zetta-tests --gtest_output=xml:build/static-proof.xml
ctest --test-dir build -R '^zetta-refused-start-exit$' --output-on-failure
./build-shared/tests/zetta-tests --gtest_output=xml:build/shared-proof.xml
ctest --test-dir build-shared -R '^zetta-refused-start-exit$' --output-on-failure
```

Static result summary:

```text
[==========] 2536 tests from 518 test suites ran. (424948 ms total)
[  PASSED  ] 2520 tests.
[  SKIPPED ] 16 tests, listed below:
```

Shared result summary:

```text
[==========] 2536 tests from 518 test suites ran. (408638 ms total)
[  PASSED  ] 2520 tests.
[  SKIPPED ] 16 tests, listed below:
```

The separate refused-start process regression passes in both builds:

```text
100% tests passed, 0 tests failed out of 1
```

Both full runs pass all six solution journeys and all 18 solution-registry
tests, including every S-1, S-2, S-3, S-4, and S-12 test. The same 16 built-in
skips cover ten optional checkpoint tests, three corpus input/output tests,
the optional Motive demo dataset, the CUDA-only training test, and the
search-engine-absent fallback test in this engine-enabled build. There are
no failures, isolated reruns, or host-contention exceptions; neither #959 nor
#1124 was needed. Both full suites also pass the schema drift checks after
regeneration.

Linux proof deferred: the work order's external Linux proof helper is absent.
