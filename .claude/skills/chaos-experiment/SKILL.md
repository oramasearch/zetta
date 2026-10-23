---
name: chaos-experiment
description: Design and run a chaos experiment against a zetta cluster — inject one fault, assert a recovery invariant with a bounded deadline. Use when the user says "chaos test", "chaos experiment", "fault injection", "partition a node", "kill a node and see what happens", "make #835 reproducible", "why does convergence stall", "the cluster looked healthy but the answer was short", or wants a silent convergence stall provoked on demand instead of caught at 1-in-N. Enforces the hypothesis format (fault · invariant · deadline · blast radius), the safety rules for a live cluster, and the rule that a green chaos run proves nothing unless the fault is proven to have landed.
---

# chaos-experiment — torment the cluster, assert it heals

zetta's suite has ~1,242 unit cases plus the in-app catalog, and **every one of the
three convergence bugs found so far was found by accident**: the `tick()` watermark
([#837](https://github.com/oramasearch/zetta/issues/837)) survived 1,491 in-process
tests, a node silently ceasing to apply inbound `SYNC`
([#835](https://github.com/oramasearch/zetta/issues/835)) was filed as "flakiness",
and the same defect on the live AKS cluster
([#841](https://github.com/oramasearch/zetta/issues/841)) was confirmed pre-existing
only by an A/B across two digests. The motivating issue is
[#842](https://github.com/oramasearch/zetta/issues/842).

A chaos experiment closes that gap: it **injects one fault and asserts a recovery
invariant with a bounded deadline**, so a silent convergence stall becomes
reproducible on demand rather than something to re-run and hope about.

Follow the **zetta-dev** skill for all coding conventions while doing this, and read
[`docs/testing/README.md`](../../../docs/testing/README.md) +
[`docs/cluster/replication.md`](../../../docs/cluster/replication.md) before you start.

## Core principle: assert that it HEALS, not that it works

**Our tests assert that things converge. They never assert that things converge when
something goes wrong.** That is the entire defect class.

A silent convergence stall is the most severe bug this engine can have: every node
reports Ready, membership is full, nothing logs an error, and a reader on the wrong
node gets a quietly incomplete answer. That is indistinguishable from data loss from
the outside. An undisturbed convergence test cannot see it — the fault never happens,
so the healing is never exercised.

So do not write "N nodes agree." Write "N nodes were driven apart, and agreed again
within D seconds."

## Every experiment is a falsifiable hypothesis

Write this block first, in the case's doc comment and in its runbook, **before any
code**. If you cannot fill in every line, the experiment is not designed yet.

```
HYPOTHESIS   a node partitioned for 30s while peers accept writes
             rejoins and converges to the identical document within 20s
FAULT        one-way partition, node 2, 30s
LOAD         4 writers, 50 keys/s, throughout
INVARIANT    all nodes report identical document version AND identical
             materialized projection within 20s of the partition healing
DEADLINE     20s (patient poll, never a fixed iteration count)
BLAST RADIUS one node, self-healing, no state left behind
```

Rules that make the block worth writing:

- **One fault per experiment.** Two faults at once and a failure names nothing.
- **The invariant is about recovery**, stated as an observable read on every node —
  not "no crash", not "no error logged".
- **The deadline is a patient poll with a wall-clock bound** (poll for seconds until
  the deadline), never a fixed iteration count. Multi-node cases here run real
  Corosio TCP in-process and are timing-bound —
  [`tests/CLAUDE.md`](../../../tests/CLAUDE.md) already makes this a rule.
- **The blast radius is stated in the experiment**, not left to the reader.

## The fault catalogue

Ordered by how likely each is to catch the known bugs. **Every one of these runs
under sustained concurrent write load** — the [#835](https://github.com/oramasearch/zetta/issues/835)
repro needs load; an idle cluster hides the defect.

**Network**
- partition a node — **one-way and symmetric are different faults**; a one-way
  partition is closer to what #835 looks like (still sending, not applying)
- heal the partition and start the deadline clock at the heal
- delay / jitter on the peer link; drop a fraction of packets
- sever the link **mid-`SYNC`**, not between syncs
- saturate the link while writes continue

**Node lifecycle**
- `kill -9` vs a graceful stop — different code paths, different residue
- restart with an intact data dir vs a wiped one
- **SIGSTOP-pause long enough to look dead, then resume** — see the pause/resume trap
  below; this one has already found a real bug

**Timing**
- clock skew between peers
- a slow disk
- a node that ticks far slower than its peers

**Membership**
- join / leave during a write
- a rejoin carrying a stale document
- two nodes writing the same key across a partition

## The invariants to assert after the fault clears

Each one gets a deadline. Assert them on **every node**, not on the initiator — #837
was found only when one assertion was forced to run from a peer instead of the node
that issued the write.

1. **Identical document version AND identical materialized projection.** Assert
   **both, always**. The `tick()` watermark bug lived exactly in the gap between them:
   a peer merge landing mid-tick was credited as applied without ever being
   reconciled, so the version moved and the projection did not. A node sat at 8 of 12
   `part:` entries permanently. Asserting only the version is how that bug survives.
2. **No key lost.** Every key written during the fault window is readable afterwards.
3. **No removed key resurrected** — a peer's gossip merge must not bring a dropped
   key back ([#838](https://github.com/oramasearch/zetta/issues/838)).
4. **A partitioned dataset stays readable exactly once** — no chunk double-counted,
   none dropped.
5. **A scatter-gather `SELECT` is complete or loudly failed, never short**
   ([#837](https://github.com/oramasearch/zetta/issues/837),
   [`docs/data/scatter-gather-select.md`](../../../docs/data/scatter-gather-select.md)).
   A short answer reported as complete is the worst outcome in the engine.
6. **Membership converges to the true roster** — the count and the identities, on
   every node.

## Where an experiment can run

Three venues, increasing realism and decreasing control. Pick the cheapest one that
can express the fault, and say in the runbook why the others were not used.

| Venue | What it gives | What it cannot do |
|---|---|---|
| **In-process `testkit::LoopbackCluster`** (`include/zetta/testkit/harness.hpp`), or the **L1 chaos harness** (`tests/cluster/chaos_harness.hpp`, [`docs/testing/chaos-experiments.md`](../../../docs/testing/chaos-experiments.md)) | fast, seeded, real TCP + mTLS, full read access to every node's document; the harness adds frame drop / delay / duplicate / reorder, partitions, abrupt stop + restart, and a lag-based timing skew | no process kill (`kill -9` is modelled as an abrupt networking stop), no real wall-clock skew, no slow disk |
| **The in-app `zetta::testkit` catalog** ([`docs/testing/test-suite.md`](../../../docs/testing/test-suite.md)) | runs *inside a deployed binary*, reachable over `testsuite_run` / the Operations → Test suite panel, so the fault is exercised against the shipped build | a panel run must **never** leave a node wedged — scoped, self-healing faults only |
| **A live k8s cluster via the operator e2e** ([`docs/testing/e2e-integration.md`](../../../docs/testing/e2e-integration.md), driven by the operator's `ZettaClusterE2ETest` CRD) | real network, real mTLS, real pod lifecycle — **the only place #841 has reproduced** | slow, sandbox-only, and destructive faults need explicit approval |

**Do not wedge the host process's node.** A `LoopbackCluster` stands up its own nodes
in their own temp directories and tears every one down on destruction; the desktop
session or the deployed node serving the panel is never a member and must never be
touched.

## The cheapest safe fault: a node with no tick loop is already "dead"

Discovered while building
[`cluster.import_participant_loss`](../../../docs/testing/runbooks/test-cases/cluster.import_participant_loss.md):
**"dead" is the absence of a thread, not a broken node.** A node participates by
running a tick loop — `tick()` is what drains the `run.launch` queue, reconciles the
projection, and does the work. A node that is simply never given a loop stays in the
roster, stays connected, stays a collective participant, and does nothing. Nothing is
broken, so nothing needs healing.

That makes it **safe, scoped and self-healing by construction rather than by
cleanup** — no fault injection required. Start here for any in-process experiment
about a participant that stops contributing. Reach for heavier machinery only when
the hypothesis genuinely needs it (a partition where the node keeps *sending*, for
instance, cannot be modelled this way).

## The pause/resume trap — "died" and "came back" are different faults

Already found, already real: a node that was merely **paused** and then resumed is
not a node that died. It wakes, **drains the launch it had queued, and re-adopts a
table after the cleanup already ran** — a late launch beating a completed cleanup.

Tell the two apart explicitly in the hypothesis and in the case id. An experiment
that ticks a paused node during the convergence wait has silently changed which fault
it is testing, and its result no longer means what its name says.

## Steps

1. **Write the hypothesis block.** Fault, load, invariant, deadline, blast radius.
   Name which known bug (or which class) it would catch.
2. **Pick the venue** from the table and justify it in one line.
3. **Stand up the cluster and the load first, with no fault**, and confirm it
   converges. A chaos experiment whose baseline is already red teaches nothing.
4. **Inject exactly one fault**, and **record evidence that it landed** (see
   Reporting discipline).
5. **Heal the fault** and start the deadline clock at the heal, not at injection.
6. **Poll the invariant patiently to the deadline** — read from *every* node, assert
   version *and* projection, plus the other invariants the hypothesis names.
7. **Assert the cleanup**: no residue, no wedged node, the roster back to the truth.
8. **Write the runbook** under
   [`docs/testing/runbooks/test-cases/`](../../../docs/testing/runbooks/test-cases/README.md)
   — one page per case, carrying the hypothesis block verbatim, what it asserts, what
   it deliberately does *not* assert, and the metrics it reports. Add it to that
   index, add the case to the [`docs/testing/test-suite.md`](../../../docs/testing/test-suite.md)
   table, and add new source files to
   [`docs/architecture/code-map.md`](../../../docs/architecture/code-map.md) — all in
   the same commit.

## Safety rules (non-negotiable)

- **Bounded blast radius, stated in the experiment.** Which nodes, for how long, what
  state is touched.
- **Always self-healing.** The fault clears on its own or in a destructor — never on
  a cleanup step that a failure path can skip.
- **Never leave a node wedged after a run.** The in-app catalog runs inside a
  deployed binary someone is using; a case that bricks the host is worse than no case.
- **Destructive faults run against a sandbox only.** Never against a shared or
  production cluster, and never against a cloud target without explicit per-action
  permission (the cloud-provider rule in `CLAUDE.md` applies in full).
- **The experiment owns its cluster.** Its own nodes, its own temp dirs, torn down on
  destruction.

## Reporting discipline: prove the fault landed

**A chaos run that passes proves nothing unless the fault actually happened.** This
is the single most common way a chaos suite lies to its owners: the partition script
silently no-ops, the pause targets the wrong pid, the load generator never started —
and the run goes green while asserting nothing at all.

So every experiment **asserts the fault occurred, before trusting the result**:

- the partition really dropped traffic (counters moved on the isolated node, or peers
  observed no progress from it)
- the paused/killed node really stopped (no tick advanced, no sync applied)
- the load really flowed (writes accepted during the fault window, counted)

Report the fault evidence as **metrics alongside the verdict**, so a reader can tell a
real pass from a vacuous one. A run whose fault evidence is absent is reported as
**Skipped or Error, never Pass**.

## Never soften an assertion to go green — extend the system

The repo's existing forcing-function rule
([`docs/testing/e2e-integration.md`](../../../docs/testing/e2e-integration.md)):
**never weaken an assertion to make it pass — extend the engine instead.** It applies
double here. Widening a deadline, dropping the projection check and keeping only the
version, or downgrading a fail to a skip converts the exact bug this suite exists to
find into a permanent blind spot. If an experiment fails, the engine is wrong until
proven otherwise — file it, do not tune it.

The genuinely-absent case is different and already has a rule: if the surface does not
exist in this build, the engine *rejects* it and the result is a tracked **Skip**,
never a silent pass.

## Where things go

```
src/core/testkit/cases_*.cpp                     in-app chaos cases (catalog in zetta-core)
include/zetta/testkit/harness.hpp                LoopbackCluster / LoopbackOptions / ScratchDir
tests/cluster/chaos_harness.hpp                  the L1 chaos harness: seeded schedule, ChaosCluster, run_step (#842)
tests/cluster/test_chaos.cpp                     L1 chaos tests, ctest label `chaos` (real mTLS, in-process)
include/zetta/cluster/frame_faults.hpp           FrameFaultInjector — drop/delay/duplicate/reorder inbound frames
tests/e2e/zetta-e2e.sh                           live-cluster level-3 checks
docs/testing/runbooks/test-cases/<id>.md         YOUR runbook, one per case
docs/testing/test-suite.md                       the catalog table
docs/cluster/replication.md                      capture / gossip / apply — what you are stressing
```

New source files go in `CMakeLists.txt` and new test files in `tests/CMakeLists.txt`
— **both are explicit lists, not globs**. A missing entry surfaces as an
undefined-symbol link error, not a compile error.

## Pitfalls

- **Asserting only the document version.** The projection can lag the version
  forever; that gap *is* #837. Assert both, on every node.
- **Asserting from the initiator.** The node that issued the write is the one most
  likely to look correct. Read back from a peer.
- **No load.** #835 does not reproduce on an idle cluster. Writes must flow during
  the fault window.
- **A fixed iteration count instead of a wall-clock deadline.** Turns a real stall
  into a flake and a slow machine into a false failure.
- **Ticking a paused node while waiting.** Silently converts a "died" experiment into
  a "came back" experiment.
- **Healing and injecting in the same measurement window.** Start the clock at the
  heal, or the deadline measures the fault duration too.
- **Two faults at once.** A failure then names nothing and the repro is not minimal.
- **A green run with no fault evidence.** The most dangerous outcome available — it
  buys confidence that was never earned.
