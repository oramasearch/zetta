# AX — agent experience

LLMs and programs driving the cluster. Agents are a first-class persona
(PER-4); AX is designed, not incidental. Its primary deployment is
**human-through-agent**: a user operating zetta from Claude Code (or another
coding agent) in their own words — the agent works the tools, the human's
stated goal rides the `intent` field, the prompts onboard the agent
progressively, and the traces + GUI live surfacing (#620) let the human see
and audit everything their agent did. Good AX is therefore also UX: every
tool description, error, and read-back is written for an agent that must
relay it to a person.

- **MCP with full parity**
  ([../api/registry-design.md](../../api/registry-design.md)): every capability a
  schema-validated tool whose description is one agent-actionable sentence
  stating side effects; **prompts as progressive disclosure** (orientation →
  use-case runs → journeys → investigation → feedback) so an agent never
  needs the full tool table to start.
- **`llms.txt` progressive-disclosure endpoints** — built
  ([#645](https://github.com/oramasearch/zetta/issues/645),
  [../../api/llms-endpoints.md](../../api/llms-endpoints.md)): the node serves
  its own agent-readable documentation over plain HTTP — `/llms.txt` (what
  this cluster is, the first three reads, where everything lives) →
  `/llms/<topic>.txt` → `/llms-full.txt` — generated at request time from the
  live registry's own descriptions, embedding **live cluster facts** (node id,
  indexes, catalog entries); never a second prose source. Custom beyond the
  community convention wherever AX demands it.
- **The spec index** — built
  ([#646](https://github.com/oramasearch/zetta/issues/646),
  [../../api/specs-index.md](../../api/specs-index.md)): `/v1/specs` +
  `list_specs`/`get_spec` tools serving the OpenAPI document plus **every**
  JSON Schema in the system (capability schemas + all 92 schema-as-code
  boundary schemas, served from their C++ accessors) as one navigable index
  of objects — discovery without reading source, drift-checked through the
  same table the build enforces.
- **Observability threaded back**: every result carries
  trace/span/duration; `get_trace` shows intent · plan · performance · result
  shape including per-node spans of distributed work — an agent can always
  answer "what actually happened?"
  ([../architecture/observability.md](../../architecture/observability.md)).
- **Solution health is queryable**: `get_monitor` reads current input and score
  drift, calibration, realized cohort performance, declared guardrails, active
  breaches, and the durable retrain request by solution name or monitor UUID.
  MCP and REST return the same schema-validated record, and the agent tool is
  read-only ([monitor.md](../../solutions/monitor.md), #1106).
- **Solution value is evidence-backed**: `get_roi_readout` reads the configured
  graph's correctness result, aggregate experiment lift with confidence
  intervals, customer-supplied economics, guardrails, agent operations, and a
  redacted audit extract. Without measured holdout outcomes it explicitly
  refuses an incrementality or ROI claim
  ([roi-readout.md](../../solutions/roi-readout.md), #1108).
- **Errors an agent can act on**: domain failures return the engine's
  path-qualified reason verbatim; consent challenges name exactly what an
  approval would permit — and can never be satisfied by the calling key
  itself.
- **Experiments before actions**: `create_experiment` defines seeded allocation
  tiers, `assign` records the entity before returning any proposed action and
  suppresses model-driven action for protected tiers, and `get_experiment`
  exposes balance, exemption erosion, and the assignment rows used by uplift
  training ([experiments.md](../../solutions/experiments.md), #1105).
- **Data evidence before methodology choice**: `analyze_dataset` reports semantic
  types, grain, keys, joins, and leakage findings; `plan_features` turns that evidence
  into explicit features and exclusions. When the prediction unit is wrong or a
  required field is absent, the read returns a named `transform_required` correction
  and proposed SQL or clean step. It never changes the dataset for the agent
  ([featurization.md](../../data/featurization.md), #1062). All 21 methodology
  callbacks enforce the plan before training and refuse selected unsafe features;
  `get_run_report.features` lists used sources/transforms and exclusions with reasons.
- **Methodology discovery** ([#1061](https://github.com/oramasearch/zetta/issues/1061)):
  `list_methodologies` exposes the 21-entry portfolio, required inputs, outputs,
  metrics, and runnable status. `compile_intent` accepts a methodology or
  solution selector; M-1 through M-21 are runnable, while unsupported work names
  its implementation issue. `run`
  refuses unsupported model kinds, and `get_graph` / `get_project` expose each
  pipeline's methodology and runnable status. M-15 compiles and runs its own
  bandit shape against the connect step's table, then reports policy metrics
  and its output tables. See
  [the registry contract](../../ml/methodologies.md).
  Default pipeline names also identify the method: M-3 returns
  `Segmentation / clustering pipeline`; M-1 retains `churn skeleton`.
  Explicit caller names are unchanged.
- **Solution discovery and composition** ([#1102](https://github.com/oramasearch/zetta/issues/1102)):
  `list_solutions` and `get_solution` expose all twelve catalogue modules and eight
  sketches with plain product copy, portfolio-number composition, data roles,
  autonomy, policy, measurement, guardrails, levels, and missing components.
  `create_solution` uses typed defaults to create a callback-built Pipeline,
  versioned policy, experiment, monitor and ROI definition on an existing project.
  Explicit snapshot, data-role, delivery and economics settings are required.
  `get_solution.solution.levels` contains objects with an integer `level`, name,
  and description; agents pass that integer to `create_solution`.
  `run_solution` executes the snapshot and methodology stages, then the policy's
  combined holdout protection, governed delivery, ledger and monitoring path.
  It returns `preview: false`; read-backs include child Run lineage and a decision
  table. S-1 has full retention defaults, named correctness refusals, reasoned
  CRM tasks, driver reports, and level-3 no-contact uplift targeting. S-2 has
  full lead-routing defaults, conversion-label and leakage refusals,
  capacity-aware CRM queues, SLA payloads, and holdout ROI. S-3 through S-12
  are marked minimal.
  See [executable solutions](../../solutions/executable-pipelines.md).
- **Runnable forecasts** ([#1043](https://github.com/oramasearch/zetta/issues/1043)):
  compile `M-4` or `S-5` with source/table and time/target/optional-series roles,
  then run the stored pipeline and poll its Run. The report carries temporal
  metrics, seasonal-naive comparison, read/excluded data and forecast table
  references. A forecast never falls back to a classifier or structural run.
  See [Running it](../../ml/forecast.md#running-it).
- **Label-free anomaly runs** (#1046): select M-7 or S-7, compile against a landed
  table, run, and poll its durable handle. Reports return the governed threshold,
  alert rate, ranked anomalies with robust contributors, output tables, and the
  columns excluded from scoring. Labels are optional evaluation data only;
  unsupported shapes, grouping, and non-causal KPI settings fail explicitly.
  See [running M-7](../../ml/anomaly.md#running-it).
- **Next-best-action runs** (#1047):
  M-8 and primary solution S-11 compile to a runnable recommendation graph;
  `run` reads that graph's table and `get_run_report` returns queryable output
  references, ranked actions, policy metrics, exclusions, and causal caveats.
- **Versioned decision policy** (#1104): `create_policy`, `get_policy`, and
  `update_policy` manage immutable policy versions; `decide` applies hard
  thresholds, tiers, budgets, caps, eligibility/exclusions, autonomy, and the
  kill-switch to one entity or a scored table. A1 results stay non-actionable
  until `approve_decision`, and every result names its policy version. See
  [the decision-policy contract](../../solutions/decision-policy.md).
- For M-13, compile hints name the image table, its image/label/source columns,
  and a registered ViT backbone. `run` reads that graph-owned table, refuses an
  unauthorized image before its bytes are read, and `get_run_report` returns
  source-disjoint accuracy, macro-F1, the confusion matrix, and prediction table.
- **Bayesian uncertainty runs** (#1056):
  M-17 is runnable: an agent can bind a landed table's entity, label, and group
  roles in compile hints, run without repeating `dataset_path`, then read
  empirical coverage, interval width, assumptions, and output tables through
  `get_run_report`.

Working checklists: the **api-dev** and **mcp-journeys** skills. Wave epic:
#614.
