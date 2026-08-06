# Pipeline docs

The intent → deployed-intelligence pipeline: a **Project** owns raw inputs (NL intent, n8n
workflows, sim-Python) that compile into the **ExecutionGraph IR** (the
connect→clean→model→index→serve→act taxonomy — the keystone everything builds on), which must pass
the **correctness gate** (RULE-1: no deploy without a passing gate) before it is **deployed**,
**executed** into a **Run**, and kept alive by the reconcile loop. Read here for anything touching
project/input/run entities, a compiler or language frontend, the gate, or deploy/execution;
governance of who may run what is [../governance/](../governance/access-control.md).

- [run-report.md](run-report.md) — what a run FOUND: the facts artifact behind a report, assembled at query time from the Model record and the at-risk table (`get_run_report`, #958).
- [run-report-prompt.md](run-report-prompt.md) — the run report artifact and the prompt that writes it: the engine computes every number, the narrator may only arrange them (#948 step 4).
- [projects.md](projects.md) — the AI project (`zetta::pipeline::Project`) data model, lifecycle CRUD, and the execution graphs it owns.
- [project-inputs.md](project-inputs.md) — project inputs (`zetta::pipeline::ProjectInput`): raw intent/artifact ingestion + provenance, replicated.
- [execution-graph.md](execution-graph.md) — the ExecutionGraph IR: typed step nodes/edges, the connect→…→act taxonomy, and its schema-validated JSON form.
- [intent-compiler.md](intent-compiler.md) — NL intent → a reviewable, editable ExecutionGraph skeleton (`zetta::pipeline::compile_intent`; templated churn mapping, FEAT-2 / #311).
- [n8n-import.md](n8n-import.md) — importing an n8n workflow JSON as an artifact input (raw bytes + sha256, schema-validated at the boundary).
- [n8n-compile.md](n8n-compile.md) — compiling an imported n8n workflow into the IR: nodes → typed steps, connections → edges (FEAT-2 / #308).
- [python-frontend.md](python-frontend.md) — the simulated-Python language frontend: parse a Python subset, lower to the IR via the engine-faithful verb table (ENT-38 / ADR-33, #520/#522).
- [datasets-shim.md](datasets-shim.md) — the sim-Python `load_dataset` shim: decode Parquet with the native reader into a (partitioned) table, IO behind the `DatasetResolver` seam (#521).
- [validation.md](validation.md) — the `ValidationReport` correctness-gate result type: per-check results, verdict, block-deploy.
- [gate.md](gate.md) — the correctness-gate runner (`zetta::pipeline::CorrectnessGate`): register checks, run them over a graph, block deploy on failure (RULE-1).
- [gate-checks.md](gate-checks.md) — the concrete graph checks: schema/type vs source profiles + data-leakage / point-in-time for ML steps (#309).
- [deployments.md](deployments.md) — placing a validated graph onto a `DeploymentTarget` (R1 local), recorded + queryable.
- [deployment-controller.md](deployment-controller.md) — the control plane: reconcile a local deployment desired→actual and self-heal a killed/failed run (FEAT-4 / TC-3).
- [solution-catalog.md](solution-catalog.md) — the AI Solution Library (`zetta::pipeline::SolutionCatalog`) + the headed Deploy panel: pick a blueprint, gate, deploy (FEAT-4 / SCR-5).
- [execution.md](execution.md) — executing a graph on a node (`zetta::pipeline::GraphExecutor`): walk steps in dependency order behind the `StepExecutor` seam, drive a Run to a terminal state.
- [runs.md](runs.md) — the Run (`zetta::pipeline::Run`) record: status / logs / outputs / lineage, with JSON-Schema boundary validation.
- [graph-plan.md](graph-plan.md) — the graph-over-cluster plan view: project how a graph would run over the current cluster (near-data / fan-out / owners) (`project_cluster_plan`, #591).
