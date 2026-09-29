# Testing docs

How zetta proves itself: the in-app **test suite** (`zetta::testkit`) — a headless-runnable catalog
of canned, parameterized cases over the real engine, including real multi-node loopback-cluster
cases — and the **example-script harness**, where the sim-Python example programs *are* the
integration tests (parse → IR → gate → run, asserting on outputs). The catalog lives in zetta-core;
the headed **Operations → Test suite** panel is a thin renderer. Read here before adding a test-suite
case or an example script, and add a runbook under `runbooks/test-cases/` alongside every new case.

Everything above is **automated** — it proves the code does what the developer intended. That is
evidence, not acceptance. `test-scripts/` holds the other half: **manual scripts a human tester
follows**, proving a real person pursuing a real goal succeeds. An issue closes on a passing script,
not on a green build.

- [mcp-journeys.md](mcp-journeys.md) — **simulated user tests**: journeys (tool sequences + read-back assertions) in three layers — in-process registry, loopback HTTP protocol, agent-driven exploratory — with the promotion rule (every discovery becomes a recorded journey).
- [audit-replication.md](audit-replication.md) — native two-process million-event audit convergence, per-node RSS/restart bounds, 100,000-key migration and REST/MCP parity (#1164), including allocation profiles and clean-build proof after #1163.
- [test-suite.md](test-suite.md) — the in-app test suite (`zetta::testkit`, #462): the catalog model, the categories (compute / DDF / ML / cluster / governance / UC-1), the "adding a case" how-to + candidate-case inventory.
- [e2e-integration.md](e2e-integration.md) — the **end-to-end integration test** (`tests/e2e/zetta-e2e.sh`): the three testing levels, the level-2 (testkit over `testsuite_run`) + level-3 (deployed-surface) coverage the e2e runs against a **live node** (incl. the authz-enforcement profile #780 — auth is always on since #799 — and the multi-node CRDT profile #781), and how it is packaged (`oramasearch/zetta-e2e`) and driven in-cluster by the operator's `ZettaClusterE2ETest` CRD (One Orama, orama-bridge#130).
- [example-scripts.md](example-scripts.md) — the sim-Python example-script harness: run an example end to end as an integration test, failures surface the offending statement (#523).
- [runbooks/test-cases/](runbooks/test-cases/README.md) — per-test runbooks: one doc per test-suite case (what it does, how to run it, options, assertions); start at its index.
- [test-scripts/](test-scripts/README.md) — **manual acceptance, for humans**: one script per issue, walking a real user through a real use case in their own language. Not automated — an issue closes only when a person has run its script and it passed.
