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

A journey is data, not ad-hoc test code — `JourneyStep{tool, input,
expect}` tables run by one driver (the example-scripts pattern:
*the journeys ARE the integration tests*):

- `expect` asserts on the result (fields present, values, `isError` + reason
  substring) and/or on a **read-back** (a follow-up read tool / `sql` whose
  rows must satisfy a predicate). Every mutating step gets a read-back — a
  journey that only checks return codes is not done.
- Failures name the failing step + assertion (the offending-statement
  discipline).
- Negative journeys are first-class: gate-blocked deploy, denied capability,
  schema-rejected input, consent-challenge-without-approval.

Canonical first three: **UC-1 happy path** (create project → intent → compile
→ gate → deploy → run → ranked at-risk list + drivers read back), **blocked
deploy** (leakage-annotated graph → gate refuses over MCP exactly as in the
GUI), **dataset ingest** (add_dataset → placement visible in
`dataset_partitions` read-back).

## Rules

- Journeys run against **real engine state** (temp data_dir, loopback
  cluster) — no mocks of zetta itself; fakes only at the CommandRunner-style
  outer seams (cloud CLIs).
- Multi-node journeys follow the patient-convergence rule (poll with a
  deadline, never a fixed count).
- Every new capability lands with at least one journey step exercising it
  (see [../api/adding-a-capability.md](../api/adding-a-capability.md)).
- A journey is one command to run locally: the gtest filter, and (for GUI
  verification) the same case surfaced in the Operations → Test suite panel.
