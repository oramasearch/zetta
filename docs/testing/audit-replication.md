# Audit segment replication proof (#1164)

Build natively with `cmake --build build -j 6`. Run one suite at a time, with
temporary data inside the worktree:

```sh
mkdir -p build/tmp
TMPDIR="$PWD/build/tmp" ./build/tests/zetta-tests --gtest_filter='AuditTrail.*:RestServerFixture.SegmentedAuditReadsHaveRestAndMcpParity'
TMPDIR="$PWD/build/tmp" ctest --test-dir build --output-on-failure -j 1
```

`AuditTrail.ReplicatedMillionEventsMeetPerNodeResidentAndStartupBudgets` launches
two native `zetta-audit-proof` processes. Each writes 500,000 real node audit
events while exchanging frames over loopback TCP. Both must reach 1,000,000
retained ids and a high-water mark of 500,000 for each origin. Neither catalog
may contain `audit:` entries. The test then stops both and starts fresh processes
against the same stores, requiring identical counts and watermarks.
After #1163, every phase also verifies that the memory registry's audit row
counts all retained events and agrees with `audit_memory_bytes`. The existing
SQL capability's REST/MCP parity test reads that `system.memory` row as well.

Every process reports `steady_rss_extra`, `startup_peak_extra`, and catalog size
in bytes, plus RSS after an additional allocator pressure-relief request.
Steady-state additional resident memory before that extra request must be below 100 MiB;
startup additional peak resident memory must be below 150 MiB. The startup
baseline is taken before `Node::start()`; the steady baseline is taken after
starting an empty node and its cluster. macOS uses Mach resident size and
`getrusage` peak RSS; Linux uses `/proc/self/statm` and `getrusage`.
Full-history materialization by an explicit SQL read is outside these idle
storage/replication bounds.

`CatalogMigrationDiscardsOneHundredThousandAuditHistories` seeds 100,000 live
legacy catalog events plus a removed audit key. Migration must retain every live
event locally under `legacy`, leave zero known audit keys/tombstones, and shrink
the saved catalog below 4 KiB while preserving an unrelated live key and removal.
Merging the stale old document must not regrow audit history. Restart and new
catalog writes must still replicate.
The incremental-reconcile regression also keeps its ordinary catalog ticks fast
after migrating 100,000 events, with concurrent segment-backed audit appends.

Other regressions cover malformed frames, missing ranges, replay deduplication,
summaries below retention low-water marks, mixed-version refusal with ordinary
catalog delivery, and identical segment-backed SQL reads over REST and MCP.
No new capability or event-value schema is introduced.

Linux proof uses the work-order's external lane when available; no containers
run on this host. The live-bus check is separate: after an all-node development
upgrade and restart, measure total process RSS below 150 MiB and verify the
expected audit history remains readable. Cloud inspection/restart requires the
repository's per-action permission; native proof does not substitute for it.

## Allocation profiling

The worker supports `ZETTA_AUDIT_PROOF_PAUSE=load` (or `restart`), which prints
the exact pid and pauses with `SIGSTOP` after convergence. This is test-only.
On macOS, enable `MallocStackLogging=1` and set `MallocStackLoggingDirectory`
inside the worktree, then inspect that pid with `heap <pid> -s`,
`vmmap --summary <pid>` and `malloc_history <pid> -allBySize`. Resume with
`kill -CONT <pid>`; the other node may then finish catch-up and pause too.

Profiling can alter residency and delay peers. Use it for allocation attribution,
then run the uninstrumented proof separately for RSS assertions. Both production
bulk boundaries and the worker's additional comparison request use
`malloc_zone_pressure_relief(nullptr, 0)` on macOS or `malloc_trim(0)` on glibc;
no allocator-retained bytes are discounted from the reported resident size.

### Allocation findings on macOS

The initial implementation converged but failed the RSS gate. An uninstrumented
comparison recorded 324,452,352 and 300,498,944 added resident bytes on the two
nodes; after pressure relief those were 324,747,264 and 300,597,248. Pressure
relief alone did not solve the failure on this host.

Paused, allocation-logged million-event snapshots from macOS 14.6.1 attributed
the retained memory as follows. Byte counts are allocator-reported allocation
sizes; MiB values are `vmmap` resident-region measurements, not budget deductions.
These allocation snapshots precede the #1163 rebase; the final clean-build RSS
proof below also includes its memory-accounting integration.

| Allocation or region | Initial implementation | Final implementation |
|---|---:|---:|
| Live heap allocations | 1,013,396 | 17,298 |
| Live allocated bytes | 94,372,432 | 55,661,760 |
| Fingerprint index, including buckets | 61,172,736 bytes | 33,554,432 bytes |
| Per-origin seek index | 25,165,824 bytes | 16,039,936 bytes |
| Segment cache plus main transport strings | 6,357,120 bytes | 4,391,040 bytes |
| Recent `AuditEvent` objects | 2,048 / 589,824 bytes | 2,048 / 589,824 bytes |
| Total resident malloc regions | 295.2 MiB | 87.4 MiB |
| Empty medium-allocation regions, resident | 164.8 MiB | 9.47 MiB |

`malloc_history` on the initial process identified 500,000 fingerprint-node
allocations from local append and another 500,000 through range receive, plus
the hash bucket array. `heap` attributed the final table to one flat allocation
and the seek index to small deque blocks. There was no per-origin duplicate
recent window or retained million-event JSON batch. The large difference between
live heap and RSS was allocator-retained transient storage; reused inflation
storage and 64 KiB range pages reduced that allocation churn.

The uninstrumented native proof on the original `dbc020bf` base reported:

```text
#1164 load node=0 events=1000000 steady_rss_extra=92078080 relieved_rss_extra=92078080 startup_peak_extra=1687552 audit_memory=54627957 catalog_bytes=34
#1164 load node=1 events=1000000 steady_rss_extra=91668480 relieved_rss_extra=91668480 startup_peak_extra=1605632 audit_memory=54627957 catalog_bytes=34
#1164 restart node=0 events=1000000 steady_rss_extra=0 relieved_rss_extra=16384 startup_peak_extra=87490560 audit_memory=54627957 catalog_bytes=34
#1164 restart node=1 events=1000000 steady_rss_extra=0 relieved_rss_extra=49152 startup_peak_extra=87687168 audit_memory=54627957 catalog_bytes=34
```

The restart worker's steady baseline is taken after loading the retained store,
so its zero steady delta is expected; `startup_peak_extra` is the restart gate.
The same native suite's append-only million-event test used 95,649,792 additional
resident bytes and 72,671,349 disk bytes, including the new seek sidecar. Against
the 85 MB append-only result reported for #1160, the replicated test no longer
introduces hundreds of megabytes of resident replication storage.

## Clean proof after rebasing onto #1163

Rebased onto `origin/dev` at `1574b656`, updated submodules, removed the generated
build directory, and configured a fresh native Release build with Ninja. Build
parallelism was capped at six jobs; tests ran serially. The audit ownership
calculation now includes both compact indexes, the segment cache, recent events,
and pending persistence, with no catalog event values.

```text
#1154 one million audit records: RSS delta 95879168 bytes, disk 72671349 bytes
#1164 migration: 100000 events, 0 audit keys/tombstones, catalog 20656577 -> 413 bytes
#1164 load node=0 events=1000000 steady_rss_extra=92667904 relieved_rss_extra=92667904 startup_peak_extra=1409024 audit_memory=54644341 audit_items=1000000 catalog_bytes=34
#1164 load node=1 events=1000000 steady_rss_extra=92405760 relieved_rss_extra=92405760 startup_peak_extra=1425408 audit_memory=54644341 audit_items=1000000 catalog_bytes=34
#1164 restart node=0 events=1000000 steady_rss_extra=0 relieved_rss_extra=65536 startup_peak_extra=87425024 audit_memory=54644341 audit_items=1000000 catalog_bytes=34
#1164 restart node=1 events=1000000 steady_rss_extra=0 relieved_rss_extra=49152 startup_peak_extra=87425024 audit_memory=54644341 audit_items=1000000 catalog_bytes=34
```

Steady-state extra RSS is 88.375 / 88.125 MiB; restart peak extra RSS is
83.375 MiB on each node. Additional pressure relief does not reduce these
steady-state readings. REST/MCP parity includes segment-backed audit reads,
protocol/watermarks, and the `system.memory` audit row.

Full native static suite command and summary:

```sh
TMPDIR="$PWD/build/tmp" ctest --test-dir build --output-on-failure -j 1
```

```text
100% tests passed, 0 tests failed out of 2619
Total Test time (real) = 639.08 sec
```

CTest registered 2,620 tests: 17 conditional skips and one disabled dispatcher
benchmark remain as configured. No flake waiver or isolated retry was needed
after the clean rebuild. The pre-rebase incremental-reconcile fixture that still
wrote `audit:` keys was corrected; its migrated-event replacement passes here.

Linux proof deferred: the external `linux-proof.sh` helper is absent on this
host. The issue does not require the shared-library build (#1096). No containers
or local images were used. The live-bus total-RSS and manual deployment check
remain unrun pending approved all-node deployment/restart and cloud inspection.
