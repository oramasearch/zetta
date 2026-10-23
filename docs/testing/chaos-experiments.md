# Chaos experiments: torment a cluster and assert it heals

[#842](https://github.com/oramasearch/zetta/issues/842). Three convergence bugs ([#837](https://github.com/oramasearch/zetta/issues/837),
[#835](https://github.com/oramasearch/zetta/issues/835), [#841](https://github.com/oramasearch/zetta/issues/841)) were found by accident, because the suite
asserted that a cluster converges when undisturbed and never that it converges *after
something goes wrong*. The chaos harness drives a live in-process cluster apart on a
seeded schedule and asserts that it comes back together. The experiment format and its
safety rules are the `chaos-experiment` skill (`.claude/skills/chaos-experiment/SKILL.md`).
This page covers the harness that runs them.

## What runs

| Piece | Where |
|---|---|
| The injection point: `zetta::cluster::FrameFaultInjector`, set through `ClusterConfig::frame_faults` | `include/zetta/cluster/frame_faults.hpp`, used by the read pump and link writer in `src/core/cluster/cluster.cpp` |
| The harness: `SeededFrameFaults`, `make_schedule`, `ChaosCluster`, `run_step` | `tests/cluster/chaos_harness.hpp` |
| The experiments: the `zetta-chaos-tests` executable, ctest label `chaos` | `tests/cluster/test_chaos.cpp` |

**The cluster.** `ChaosCluster` stands up 3 to 5 nodes on `127.0.0.1`, each with its own
`ReplicationManager` and a real `Cluster`. Every node runs **mutual TLS 1.3** from one
join token, which is the production transport. Node 0 is the seed hub. Each node publishes
its `node:<id>` entry, so the star fills in to a full mesh (#789), and a restarted node is
dialed again. The harness owns its nodes and ports, and keeps nothing on disk.

**The load.** One writer thread per node overwrites or removes (15%) its own keys
`chaos:<node>:<k>` throughout every fault window. Each key has exactly one writer, so the
harness keeps an exact **ledger** of every key's final state: the last `seq` written, or
removed.

**The faults.** Each step injects exactly one fault on one node:

| Kind | What happens | Proof that it landed |
|---|---|---|
| `load-only` | no fault (the baseline, and the #835 probe) | writes flowed during the window |
| `kill-restart` | the node's networking stops abruptly (the `io_context` stops mid-flight and every socket closes), its acknowledged document is kept, and after the window it restarts on the same port from that document (`ReplicationManager::restore`) | every survivor's peer count dropped, and the node bound its port again |
| `partition-one-way` | the node discards every frame from its peers but keeps sending: the link-level shape of #835 | frames discarded on the node |
| `partition-symmetric` | the node and its peers discard each other's frames | the same |
| `frame-drop` | each inbound frame on the node is dropped with probability p | frames dropped |
| `frame-delay` | each frame is held for a random [min, max] ms with probability p | frames held |
| `frame-duplicate` | each frame is applied twice with probability p | frames duplicated |
| `frame-reorder` | a frame is held until the next one has been applied (or for 150 ms) | frames reordered |
| `timing-skew` | the node sees every peer frame a constant lag late (bounded below the idle timeout) | frames held |

Faults are injected at the **receiver**: a frame discarded on arrival is a frame the link
lost. A peer's activity clock moves only when a frame is actually applied. Because of that,
a partitioned node sees its peer fall silent, drops the link after the idle timeout (2 s
here), redials, and goes silent again until the heal, as it would in a real partition.
`HELLO` is never filtered, because the peer is not known until it arrives.

**Why skew is a lag.** No code on the convergence path reads a wall clock: nothing in
`cluster.cpp` or `replication.cpp` calls `system_clock`, and the CRDT orders changes
causally, not by time. An in-process wall-clock offset would therefore change nothing,
and a fault that cannot change anything cannot be proven to have landed. The timing
analogue that does matter here is one node seeing its peers' state late. A wall-clock
skew experiment belongs in the live-cluster venue once something reads wall time.

**The heal invariant.** After each step the fault is healed and the load stops. The
deadline clock starts **at the heal**. The harness then patiently polls **every** node,
not only the initiator, until all of these hold:

1. every node has a full set of peers (`n-1`) and `membership()` lists all `n` nodes;
2. every node has the **same version AND the same projection** (`snapshot()`). Checking
   only the version is how #837 survived;
3. **no key lost and no key stale**: every key in every ledger is present on every node at
   its last `seq`;
4. **no key resurrected** ([#838](https://github.com/oramasearch/zetta/issues/838)): every key the ledger says was removed is absent on every node.

If the deadline (30 s) passes with the invariant still failing, the step fails. The
failure names the invariant that broke, the fault evidence, the whole schedule, every
node's state (version, entries, document bytes, peers, connections, members, writes, and
frame counters) and the **rerun line**. A failure with the **#835 shape** is flagged as
such: every link is up, the other nodes agree, and one node's document has not moved for
at least half the deadline.

**A step whose fault did not land fails** and is never counted as a pass. A green run
with no fault evidence would prove nothing.

## Reproducibility

The seed fixes the schedule (fault kind, target, window, parameters), every node id (and
so the min-node-id link tiebreak), each node's frame-fault decision sequence, and each
writer's key and op sequence. It does not fix the thread interleaving of real TCP and TLS
in-process. A seed replays the same torment, not the same nanoseconds, so a timing race
reproduces at a rate rather than every time.

## Running

```sh
cmake --build build --target zetta-chaos-tests -j 6
ctest --test-dir build -L chaos --output-on-failure        # what the default suite runs
```

The default `ctest` runs the `ChaosFaults.*` unit cases (no network).
`ChaosSmoke.ThreeNodeMtlsMeshHealsFromEveryFaultClass` takes every fault class once, on a
3-node mesh, with 1.2 s windows. It **skips** unless `ZETTA_CHAOS_SMOKE=1` is set, until #835
is fixed: it hits the #835 stall in most runs (table below), and a test that fails that often
would leave the default suite red. The long runs **skip** unless asked for too:

```sh
# the every-fault-class smoke (opt-in until #835 is fixed)
ZETTA_CHAOS_SMOKE=1 build/tests/zetta-chaos-tests --gtest_filter='ChaosSmoke.*'

# N seeded schedules of ZETTA_CHAOS_STEPS single faults on a 3–5 node mesh
ZETTA_CHAOS_SCHEDULES=50 ZETTA_CHAOS_NODES=5 ZETTA_CHAOS_STEPS=8 \
  build/tests/zetta-chaos-tests --gtest_filter=Chaos.SeededSchedulesHeal

# the #835 probe: the exact load of ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad,
# no injector installed (the network thread runs unperturbed), N fresh clusters
ZETTA_CHAOS_PROBE_ROUNDS=40 build/tests/zetta-chaos-tests --gtest_filter=Chaos.MeshLoadStallProbe
```

`ZETTA_CHAOS_SEED` sets the first seed. When it is unset, `SeededSchedulesHeal` picks one
from the clock and prints it before each schedule as `[chaos] seed=…`.

Ports: the smoke uses `43900–43902`, the schedules `43910–43914` and the probe
`43920–43922`.

## What it found on the first runs (2026-10-21)

These runs were on a macOS host under heavy build load (load average 60–80), with a
`RelWithDebInfo` build at `origin/dev` `e9587213` plus this harness.

| Run | Result |
|---|---|
| `ChaosSmoke` ×9 | 2 healed after every step; one of those then crashed in teardown (below). 4 hit the **#835 shape** on a step that varied between runs (frame-delay, reorder, timing-skew, symmetric partition). 3 failed because, as a stall began, the next fault could not be proven to land (0 frames duplicated of 7 received; survivors never saw a killed node leave) |
| `Chaos.MeshLoadStallProbe`, 15 single-round processes, no injector | **2 of 15 stalled** with the #835 shape (seeds 901, 912) |
| `ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad` on a clean `origin/dev` build, 42 repeats | 13 of 42 failed to converge: the same defect, without the harness |

The injector's counters show what #835 is not. After the heal, the frozen node's read
pump **received 0 to 3 frames in 30 s**, while its peers received 15 to 69. It is not
applying frames it was given: it is not being given frames at all. Its links stay up
through those 30 s even though the idle timeout is 2 s, and that timeout is enforced
only by the link writer. So the node's writers are blocked too: a link whose writer is
suspended in `write_frame` is never reaped.

**A second defect surfaced: tearing down a `Cluster` can overflow the stack.** That
covers `Cluster::stop()` followed by destruction, a harness `kill`, and the end of any
test. `~State` destroys the `io_context`. `kqueue_scheduler::shutdown()` then destroys
the handle of a coroutine still suspended in `FramedStream::write_frame`. That
handle's promise destroys its continuation (`link_writer`), which destroys the
`write_frame` task it is awaiting, and the two recurse about 958,000 frames deep.
Capy forbids this in `io_awaitable_promise_base`. It ended 3 of the 15 probe processes
(SIGSEGV, exit 139) after their verdict was printed, and it also ended the existing
`ClusterSecurity` mesh test. It is filed as [#1351](https://github.com/oramasearch/zetta/issues/1351).

## What it does not do yet

- **No real process kill.** `kill-restart` is an abrupt stop of the node's networking in
  the same process, with its acknowledged writes kept. There is no `SIGKILL`, no
  wiped-data-dir restart, and no `SIGSTOP` pause. The pause trap in the skill needs a node
  whose tick loop resumes, which a bare `Cluster` does not have.
- **No severing mid-`SYNC`.** Frames are whole lines above TLS. Cutting a TLS record in half
  needs a byte-level proxy, as in `PeerGateway`'s forwarders.
- **Only the replicated document.** The scatter-gather `SELECT` and partitioned-dataset
  invariants need a full `Node` and its projection. They belong in the in-app testkit venue.
- **Incremental gossip changes what drops mean.** While `SYNC` carries the whole document,
  a dropped or duplicated frame only costs time. Once gossip is incremental
  ([#794](https://github.com/oramasearch/zetta/issues/794)), a dropped delta must be
  recovered, and the `frame-drop` and `partition-*` schedules are the regression gate for that.
