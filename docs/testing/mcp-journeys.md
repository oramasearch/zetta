# MCP journeys — simulated user tests as suites

The testing paradigm the control surface unlocks (#619): **a user journey is a
test** — an ordered sequence of capability calls plus assertions on read-backs,
exactly what a design partner (or their agent) would do, executed as a suite
against a real node/cluster. Journeys test two things at once: **cluster
correctness under realistic use** and **product ergonomics** (an awkward tool
sequence is a UX bug that fails loudly here first).

## The three layers

| Layer | What runs | When |
|---|---|---|
| 1. **Registry journeys** (in-process) | Call `zetta::api::Registry` handlers directly in gtest over a real `Node` / loopback cluster — no HTTP. Fast, deterministic, the default home for every journey. | every `ctest` run |
| 2. **Protocol journeys** (loopback HTTP) | The same steps over real JSON-RPC against the embedded server — auth, schema rejection, error mapping, consent challenges. Few but real. | every `ctest` run |
| 3. **Agent journeys** (exploratory) | Claude Code (or any MCP client) drives a live node — free-form use, new-user simulation, ergonomics probing. Not CI; the discovery layer. | on demand |

**The promotion rule:** anything layer 3 discovers — a bug, a confusing
sequence, a missing read-back — is recorded as a layer-1 journey in the same
fix. Exploratory findings become permanent regressions; the suite only grows.

## Journey shape

A journey is data, not ad-hoc test code — `Journey{JourneyStep{tool, input,
expect}}` tables run by **one shared driver**, `tests/api/journey.hpp`
(the example-scripts pattern: *the journeys ARE the integration tests*):

- `expect` asserts on the result (`Check`s — equals / contains / sizes /
  any-element / a named predicate escape hatch) or, for a first-class
  negative step (`ok = false`), on the error code, the engine's reason
  substrings, and the JSON-pointer path. Read-backs are just further steps
  (a follow-up read tool / `sql` whose rows must satisfy a check). Every
  mutating step gets a read-back — a journey that only checks return codes
  is not done.
- Values flow between steps by name: `save` captures result fields (a JSON
  pointer, or the specials `trace_id`/`span_id`), and any later `"$name"`
  input or expected value resolves to the saved value.
- `kind: job` handles poll patiently: a step's `Poll{pointer, until_any_of,
  deadline}` re-issues the call (pumping `Node::tick` + prune, or
  `ApiServer::drain`) until the status is terminal — a deadline, never a
  fixed count.
- Failures name the failing step + assertion: `JourneyReport::summary()` is
  `step N (tool — description): check "…" failed: …; actual: …` (the
  offending-statement discipline).
- The driver is **transport-agnostic** (a `Caller` function): the same table
  runs at layer 1 (in-process `Registry::call`) and layer 2 (real JSON-RPC —
  `test_mcp_server.cpp` maps `structuredContent` back to an `ApiResult`).
- Negative journeys are first-class: gate-blocked deploy, denied capability,
  schema-rejected input, consent-challenge-without-approval.
- **No new JSON format**: the tables are in-code structs; the only JSON is
  capability input/output, already schema-validated at the registry boundary.

The canonical three (#619) live as tables in
`tests/api/journeys_canonical.hpp`, shared verbatim by both layers:
**UC-1 happy path** (create project → intent → compile → gate → deploy → run
→ ranked at-risk list + drivers read back, every action audited under the
acting key, and the near-data trace read-back — the train job's `get_trace`
must show the `near-data:train` span), **blocked deploy** (leakage-annotated
graph → gate/deploy/run refuse over MCP exactly as in the GUI, with
read-backs proving nothing placed and nothing executed), **dataset ingest**
(add_dataset → placement visible in `get_dataset` AND the
`system.dataset_partitions` read-back). `tests/api/test_action_journeys.cpp`
runs them at layer 1 (plus n8n import+compile, sim-Python compile,
testsuite_run submit-then-poll, and the denied-key negative);
`tests/api/test_mcp_server.cpp` replays dataset-ingest and gate-blocked over
the wire; see [../api/governed-actions.md](../api/governed-actions.md).

The methodology registry journey in `test_action_journeys.cpp` discovers all
21 entries, compiles S-1 through M-1, verifies issue-bearing refusals for M-3
and segmentation keywords, rejects an unmatched non-predictive intent, refuses
a segmentation model at run time, and reads back `methodology` plus `runnable`
from both graph and project views (#1061).

The M-7 journey lands a label-free table, creates and reads back a project,
captures intent, compiles the anomaly methodology, runs without a dataset
override, waits for completion, and verifies report metrics, exact alert count,
ranked findings, excluded columns, output tables, and lineage. It also compiles
S-7 and refuses label/feature overrides before starting a Run (#1046).
`AnomalyMethodology.OneRegistryRunLaunchesOwnedPartitionsOnBothMembers` adds a
real two-member registry run to prove coordinated launch and primary ownership.

## Rules

- Journeys run against **real engine state** (temp data_dir, loopback
  cluster) — no mocks of zetta itself; fakes only at the CommandRunner-style
  outer boundaries (cloud CLIs).
- Multi-node journeys follow the patient-convergence rule (poll with a
  deadline, never a fixed count).
- Every new capability lands with at least one journey step exercising it
  (see [../api/adding-a-capability.md](../api/adding-a-capability.md)).
- A journey is one command to run locally: the gtest filter — e.g.
  `./build/bin/zetta-tests --gtest_filter='Uc1Journey.*'` replays the UC-1
  happy path, `--gtest_filter='ActionJourneys.*'` the standalone-node set,
  `--gtest_filter='McpServerFixture.Journey*'` the layer-2 replays — and (for
  GUI verification) the same case surfaced in the Operations → Test suite
  panel.
