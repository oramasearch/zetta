# Testing docs

How zetta proves itself: the in-app **test suite** (`zetta::testkit`) — a headless-runnable catalog
of canned, parameterized cases over the real engine, including real multi-node loopback-cluster
cases — and the **example-script harness**, where the sim-Python example programs *are* the
integration tests (parse → IR → gate → run, asserting on outputs). The catalog lives in zetta-core;
the headed **Operations → Test suite** panel is a thin renderer. Read here before adding a test-suite
case or an example script, and add a runbook under `runbooks/test-cases/` alongside every new case.

- [mcp-journeys.md](mcp-journeys.md) — **simulated user tests**: journeys (tool sequences + read-back assertions) in three layers — in-process registry, loopback HTTP protocol, agent-driven exploratory — with the promotion rule (every discovery becomes a recorded journey).
- [test-suite.md](test-suite.md) — the in-app test suite (`zetta::testkit`, #462): the catalog model, the categories (compute / DDF / ML / cluster / governance / UC-1), the "adding a case" how-to + candidate-case inventory.
- [example-scripts.md](example-scripts.md) — the sim-Python example-script harness: run an example end to end as an integration test, failures surface the offending statement (#523).
- [runbooks/test-cases/](runbooks/test-cases/README.md) — per-test runbooks: one doc per test-suite case (what it does, how to run it, options, assertions); start at its index.
