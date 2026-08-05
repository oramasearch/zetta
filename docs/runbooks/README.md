# Runbooks

Step-by-step procedures for work that is done repeatedly and has to come out the same way
each time. A runbook states the commands, what each step produces, and what to check —
so the result is reproducible by someone who was not there when it was first done.

Distinct from the domain docs, which describe how something *works*, and from
[reports/](../reports/README.md), which record what a run *found*.

- [regenerating-the-run-report.md](regenerating-the-run-report.md) — reproduce every figure
  in the churn reports from a clean checkout: generate the dataset, run the pipeline to emit
  its facts artifact, write the prose from those facts, and refresh the diagrams. Pairs with
  the **run-report** skill, which enforces the never-compute rule mechanically.

Domain-specific runbooks live with their domain — the agent's training, inference and
corpus-diversity runbooks are under [`docs/agent/`](../agent/README.md), and the connector
and Snowflake procedures under [`docs/data/`](../data/README.md).
