# Runbooks

Step-by-step procedures for work that is done repeatedly and has to come out the same way
each time. A runbook states the commands, what each step produces, and what to check —
so the result is reproducible by someone who was not there when it was first done.

Distinct from the domain docs, which describe how something *works*, and from
[reports/](../reports/README.md), which record what a run *found*.

- [from-clone-to-deployed-model.md](from-clone-to-deployed-model.md) — the whole customer
  path on `v0.1.0`: build (OpenSSL is the first gate, on every platform) → run a node →
  connect a coding agent over MCP → train a model on your own data → `deployment_profile`
  → a Helm chart. Every command and number in it was executed against a live node, including
  the measurement that REST and MCP return the same figure to the last digit. Carries the
  traps that stop an evaluator: the consent gate refusing a *local* Postgres, a compiled
  Pipeline sitting `proposed` until `approve_plan`, `--listen` being the peer port, and the
  complete build-time egress list a locked-down build template will ask for.
  Warehouse entity columns may use text business identifiers; Runs, outputs and
  deliveries retain those values.
- [regenerating-the-run-report.md](regenerating-the-run-report.md) — reproduce every figure
  in a churn run report from a clean checkout: generate the dataset, run the pipeline to emit
  its facts artifact, write the prose from those facts, and refresh the diagrams. Pairs with
  the **run-report** skill, which enforces the never-compute rule mechanically.

Domain-specific runbooks live with their domain — the agent's training, inference and
corpus-diversity runbooks are under [`docs/agent/`](../agent/README.md), and the connector
and Snowflake procedures under [`docs/data/`](../data/README.md).
