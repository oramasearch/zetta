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

Both native build trees were deleted and configured afresh after the final
#1100 refresh (`bd9d94d`). The counts below describe that refreshed tree.

Native static and shared-dependency suites are run separately with at most
six build jobs. The complete `zetta-tests` binary supplies the proof counts;
the only pre-authorized timing waivers are #959 and #1124. OpenAPI and boundary
schemas are regenerated with `ZETTA_WRITE_SCHEMAS=1`, then drift-checked.

The maintainer's closing decision makes the static build the acceptance proof;
the shared-library build is supplemental, not required (#1096). Static peer-wait
failures are accepted with the isolated checks and #959/#1124 waivers below.
The maintainer also verified forecasting 3/3 on this static build at load about
10, as recorded on [#1124](https://github.com/oramasearch/zetta/issues/1124#issuecomment-5879091428).
The failed shared forecasting rerun is retained as evidence, not treated as a
passing result or a remaining delivery gate.

| Full run | Tests | Passed | Skipped | Failed | Acceptance |
|---|---:|---:|---:|---:|---|
| Native static | 2,541 | 2,520 | 16 | 5 | Accepted with isolated checks and waivers |
| Native shared | 2,541 | 2,518 | 16 | 7 | Supplemental; not required |

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
cmake --build build-shared --target boost_beast2 zetta-tests zetta-refused-start-test -j 6
```

Schema regeneration and focused proof:

```sh
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests \
  --gtest_filter='Schemas.*:OpenApi.*:SolutionRegistry.*:SolutionJourney.*:CorrectnessGate.*:GraphChecksThroughGate.*' \
  --gtest_output=xml:build/focused.xml
```

```text
[==========] 43 tests from 6 test suites ran. (59527 ms total)
[  PASSED  ] 43 tests.
```

Static proof and supplemental shared run, executed one suite at a time:

```sh
./build/tests/zetta-tests --gtest_output=xml:build/static-proof.xml
ctest --test-dir build -R '^zetta-refused-start-exit$' --output-on-failure
./build-shared/tests/zetta-tests --gtest_output=xml:build/shared-proof.xml
```

Static result summary:

```text
[==========] 2541 tests from 520 test suites ran. (469068 ms total)
[  PASSED  ] 2520 tests.
[  SKIPPED ] 16 tests, listed below:
[  FAILED  ] 5 tests, listed below:
[  FAILED  ] SurvivalHazardPartitions.DistributedTrainingMatchesSingleNode
[  FAILED  ] GraphPartitions.DistributedAllreduceMatchesTheSingleMemberReferenceAcrossThreeMembers
[  FAILED  ] ActiveLearning.UncertaintyWithDiversityBeatsRandomAtTheSameBudget
[  FAILED  ] NextBestActionPartitions.DistributedMatchesSingleMember
[  FAILED  ] ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad
```

Shared result summary:

```text
[==========] 2541 tests from 520 test suites ran. (425875 ms total)
[  PASSED  ] 2518 tests.
[  SKIPPED ] 16 tests, listed below:
[  FAILED  ] 7 tests, listed below:
[  FAILED  ] ActiveLearning.UncertaintyWithDiversityBeatsRandomAtTheSameBudget
[  FAILED  ] AuditTrail.ReplicatesToPeer
[  FAILED  ] IsolationForestPartitions.DistributedForestMatchesTheSingleMemberOracle
[  FAILED  ] ForecastPartitions.SeveralMembersMatchTheSingleMemberModel
[  FAILED  ] SegmentationCluster.ThreePartitionsMatchTheSingleMemberCentres
[  FAILED  ] ClusterRouting.MembershipSurvivesPeerBlip
[  FAILED  ] DropReplication.ADroppedTableStaysDroppedOnBothNodes
```

The separate refused-start process regression passes in the static build:

```text
100% tests passed, 0 tests failed out of 1
```

Both full runs pass all six solution journeys and all 18 solution-registry
tests, including every S-1, S-2, S-3, S-4, and S-12 test. Their 16 built-in
skips cover ten optional checkpoint tests, three corpus input/output tests,
the optional Motive demo dataset, the CUDA-only training test, and the
search-engine-absent fallback test in this engine-enabled build. Schema drift
checks pass after regeneration.

### Static full-run failures and isolated checks

Four failures in unchanged distributed-methodology tests pass individually.
Under this work order they are host-contention exceptions, not new waivers.
The fifth failure is the explicitly permitted [#959](https://github.com/oramasearch/zetta/issues/959)
waiver; that issue documents intermittent failures both in full runs and in
isolation. The TLS rerun also failed and is not counted as passing.
The subsequent closing decision reports that all five static failures pass
individually and explicitly accepts the static proof with #959 covering TLS.
The table below preserves the results observed during this work order, rather
than replacing the earlier TLS result with the later reported verification.

Load at the start of the full run was `{ 16.23 65.16 56.59 }`. Readings after
the four methodology failures surfaced and after the TLS failure were:

```text
{ 14.55 43.20 48.97 }
{ 3.34 18.13 35.49 }
```

Each isolated command used the same PostgreSQL and temporary-directory
environment, with `--gtest_filter` set to exactly one test. The one-minute
load was below 40 before every rerun:

| Test | `sysctl -n vm.loadavg` before rerun | Isolated result |
|---|---|---|
| `SurvivalHazardPartitions.DistributedTrainingMatchesSingleNode` | `{ 3.06 17.58 35.09 }` | 1 passed |
| `GraphPartitions.DistributedAllreduceMatchesTheSingleMemberReferenceAcrossThreeMembers` | `{ 3.31 17.15 34.74 }` | 1 passed |
| `ActiveLearning.UncertaintyWithDiversityBeatsRandomAtTheSameBudget` | `{ 3.27 16.68 34.36 }` | 1 passed |
| `NextBestActionPartitions.DistributedMatchesSingleMember` | `{ 3.83 15.52 33.33 }` | 1 passed |
| `ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad` | `{ 4.90 15.18 32.90 }` | 1 failed; #959 waiver |

Isolated output:

```text
[       OK ] SurvivalHazardPartitions.DistributedTrainingMatchesSingleNode (4342 ms)
[       OK ] GraphPartitions.DistributedAllreduceMatchesTheSingleMemberReferenceAcrossThreeMembers (2156 ms)
[       OK ] ActiveLearning.UncertaintyWithDiversityBeatsRandomAtTheSameBudget (17694 ms)
[       OK ] NextBestActionPartitions.DistributedMatchesSingleMember (6996 ms)
[  FAILED  ] ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad (22690 ms)
```

Initial failure excerpts (worktree prefixes omitted):

```text
tests/ml/test_survival.cpp:183: Failure
Value of: wait_for([&] { return first.cluster.peer_count() == 1 && second.cluster.peer_count() == 1; })
  Actual: false
Expected: true

tests/ml/test_graph_partitions.cpp:120: Failure
Value of: wait_for([&] { for (const auto &member : members) { if (member->cluster.peer_count() != members.size() - 1) { return false; } } return true; })
  Actual: false
Expected: true

tests/ml/test_label_efficiency.cpp:100: Failure
Value of: wait_for([&] { for (const auto& node : nodes) { if (node->cluster.peer_count() != count - 1) { return false; } } return true; })
  Actual: false
Expected: true

tests/ml/test_next_best_action_partitions.cpp:136: Failure
Value of: wait_for([&] { for (const auto &member : result) { if (member->cluster.peer_count() != count - 1) { return false; } } return true; })
  Actual: false
Expected: true

tests/cluster/test_cluster_security.cpp:263: Failure
Value of: [&] { for (int waited = 0; waited < 20000; waited += 50) { if (meshed()) return true; sleep_ms(50); } return meshed(); }()
  Actual: false
Expected: true
mesh did not form over mTLS
```

Active learning and next-best-action also reported downstream comparison
failures after their failed peer waits. Full output remains in
`build/static-proof.log`; the individual logs and XML reports use the
`build/static-{survival,graph,active-learning,next-best-action,tls}` prefixes.
The reviewer reruns the full suite without competing workers before merging.

### Supplemental shared-run checks

Shared active learning, audit replication, and isolation forest pass when run
individually. Shared forecasting still fails its peer-count wait and produces
7 forecast rows instead of 14. That failure is waived for this landing under
the maintainer's #1124 decision; the static forecasting verification passes
3/3. The remaining shared-only follow-ups are not required by the closing
decision. The shared TLS test passes in the full run.

| Shared isolated test | `sysctl -n vm.loadavg` before rerun | Result |
|---|---|---|
| `ActiveLearning.UncertaintyWithDiversityBeatsRandomAtTheSameBudget` | `{ 10.22 9.69 21.38 }` | 1 passed |
| `AuditTrail.ReplicatesToPeer` | `{ 12.61 10.29 21.12 }` | 1 passed |
| `IsolationForestPartitions.DistributedForestMatchesTheSingleMemberOracle` | `{ 12.61 10.29 21.12 }` | 1 passed |
| `ForecastPartitions.SeveralMembersMatchTheSingleMemberModel` | `{ 12.64 10.33 21.08 }` | 1 failed; #1124 waiver |

```text
[       OK ] ActiveLearning.UncertaintyWithDiversityBeatsRandomAtTheSameBudget (16727 ms)
[       OK ] AuditTrail.ReplicatesToPeer (548 ms)
[       OK ] IsolationForestPartitions.DistributedForestMatchesTheSingleMemberOracle (3374 ms)
[  FAILED  ] ForecastPartitions.SeveralMembersMatchTheSingleMemberModel (11469 ms)
```

Shared failure excerpts (worktree prefixes omitted):

```text
tests/ml/test_label_efficiency.cpp:127: Failure
Expected equality of these values:
  jobs[index]->status()
    Which is: 4-byte object <03-00 00-00>
  JobStatus::Succeeded
    Which is: 4-byte object <02-00 00-00>
label efficiency: active-model allreduce failed

tests/governance/test_audit.cpp:262: Failure
Value of: converged
  Actual: false
Expected: true

tests/ml/test_anomaly_partitions.cpp:289: Failure
Value of: wait_for_cluster(members)
  Actual: false
Expected: true

tests/ml/test_forecast_runner.cpp:119: Failure
Value of: wait_for([&] { return std::ranges::all_of(members, [&](const auto &member) { return member->cluster.peer_count() == members.size() - 1; }); })
  Actual: false
Expected: true

tests/ml/test_segmentation.cpp:164: Failure
Value of: wait_for_cluster(members)
  Actual: false
Expected: true

tests/cluster/test_cluster_routing.cpp:337: Failure
Value of: wait_for([&] { return a.nodes().size() == 1; }, 6000)
  Actual: false
Expected: true

tests/cluster/test_drop_replication.cpp:150: Failure
Value of: converge(pair.a, pair.b, [&] { auto db_b = pair.b.find_database("sales"); return db_b && db_b->find_table("orders"); })
  Actual: false
Expected: true
the table never reached the peer
```

Load at shared-run start was `{ 5.40 13.22 30.66 }`. Readings as failures were
observed were `{ 8.25 11.94 27.26 }`, `{ 5.34 10.20 25.22 }`,
`{ 8.12 9.75 23.61 }`, `{ 8.04 9.36 22.29 }`, and `{ 10.22 9.69 21.38 }`.
Full output remains in `build/shared-proof.log`; the individual logs and XML
reports use the `build/shared-{active-learning,audit,isolation-forest,forecast}`
prefixes. No test implementation or assertion was weakened for this landing.

Linux proof deferred: the work order's external Linux proof helper is absent.
