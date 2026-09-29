# zetta docs

zetta is Orama Zetta's core — a database node that runs **headed** (SDL3 + Dear
ImGui desktop UI) or **headless**. This directory documents the system; keep it
in sync as the system changes (docs land in the same commit as the change).

The docs are organized for **progressive disclosure**, by domain:

1. **This index** — pick the domain.
2. **The domain's `README.md`** — a short overview + a one-line index of its
   pages. Read it before working in that domain.
3. **The page** — one page per shipped slice, with symbols, issue refs, and
   boundaries.

Orientation shortcuts: [full-context.md](full-context.md) is the **reload
map** (the whole project's where-to-look, kept current);
[product/orama-zetta.md](product/orama-zetta.md) is the
**read-first** product framing; [architecture/architecture.md](architecture/architecture.md)
is the system narrative; [architecture/code-map.md](architecture/code-map.md) is
the annotated file-by-file map of the codebase; [build.md](build.md) is build /
run / test.

## Domains

Pub/sub [CRUD](pubsub/crud.md) covers node-local topic/message management on
REST and MCP, non-consuming reads, revisions, retention and mutation auditing;
[dispatch](pubsub/dispatch.md) covers the shared topic-sized worker pool.
The [audit trail](governance/audit-trail.md) uses per-origin segment replication
outside the catalog, suppresses empty polling windows, retains summaries on an
independent schedule, and supports an admin-only recorded purge, with
[native convergence and memory proofs](testing/audit-replication.md).

| Domain | Covers |
|---|---|
| [product/](product/README.md) | What Orama Zetta is: the read-first framing + vision (Intelligence Velocity), personas, positioning, the use-case catalog, control surfaces, and the per-audience experience contracts (UX · DX · AX · serving). |
| [architecture/](architecture/README.md) | The node, run modes, module map, the code map, schema-as-code, the async runtime interface, true hybrid, the living codebase health audit. |
| [data/](data/README.md) | Storage engines and the on-disk format, large objects, datasets, partitioning (interface, write path, rebalance, sharded placement), the native Parquet reader, the query layer. |
| [cluster/](cluster/README.md) | Equal-peer clustering, CRDT replication, the signal plane, collectives, per-node telemetry. |
| [compute/](compute/README.md) | The `ComputeDevice` accelerator interface and backends, work-stealing scheduler, job coordinator, compute jobs, autoscaling, checkpoints, compute-near-data. |
| [ddf/](ddf/README.md) | The distributed dataframe: the frame core and its local + distributed ops (scan/filter/project, sort, join, group-by, as-of, featurize). |
| [ml/](ml/README.md) | Native ML: histogram GBDT, document extraction, calibration, batch scoring / rank-at-risk, exact TreeSHAP, the Model entity, live Run metrics, and the native correctness oracle. |
| [solutions/](solutions/README.md) | The S-1 through S-12 registry, Band-B sketches, executable composed Pipelines, snapshots, incremental replicated projections, and shared audit, policy, experiment, monitoring, action delivery and ROI foundations. |
| [agent/](agent/README.md) | The native RLVR agent (#555 base, #853 per-customer): the deterministic reward grader, and — as they ship — rollouts and the training loop. The model Orama post-trains and ships. Its data + model [cards](agent/cards/README.md). |
| [dataset/](dataset/README.md) | Native, in-Zetta training-dataset generation (#873): the `DatasetGenerator` interface + factory (base-agent + Auto-Suggest), plan-grounded + admission-gated — the native replacement for the offline Python corpus generators, and what a per-customer fork grows its own corpus with. |
| [autograd/](autograd/README.md) | zetta's own reverse-mode autodiff engine (native C++26, no foreign runtime): the `Tensor` value graph + `backward()`, the Qwen3-parity model, checkpoint I/O, and the RLVR training math the agent is built on. |
| [tokenizer/](tokenizer/README.md) | The native byte-level BPE tokenizer (GPT-2 / Qwen family): text ⇄ token ids for the model — UTF-8 codec + byte↔unicode map, pre-tokenizer, merges, vocab, HF-format loading. |
| [pipeline/](pipeline/README.md) | AI projects and their inputs, the ExecutionGraph IR, the compilers (intent / n8n / sim-Python), the correctness gate, deploy + the reconcile control plane, runs, the solution catalog. |
| [indexing/](indexing/README.md) | The pluggable `IndexBackend` interface, the oramacore-cpp full-text backend, the index step executor, distributed full-text. |
| [pubsub/](pubsub/README.md) | In-memory pub/sub (#1024): addressed events, inboxes, runtime topics, governed delivery options, detected limits, spilling, restart survival. Design: [plans/pubsub/](plans/pubsub/README.md). |
| [api/](api/README.md) | The programmatic control surface: the capability registry, the MCP server + REST rendering, auth/consent, the main-thread bridge. |
| [cloud/](cloud/README.md) | The `Provisioner` interface, the seven provisioner backends (local / Azure / AWS / GCP / OCI / k8s / bare-metal), the CLI control-plane pattern, burst identity, cost-cap approval, the cloud inspector. |
| [governance/](governance/README.md) | Key-scoped access control, the audit trail, the governed console. |
| [workspace/](workspace/README.md) | The Workspace Profile — the environment/role/daily-work + approved-source-refs entity behind the "Discover Opportunities" pathway (#886). |
| [billing/](billing/README.md) | Metered usage + plan: the live `system.usage` snapshot (metered from engine state) and the Plan quota/tier entity it is measured against (Conflict D). No payment processor — on-prem. |
| [ops/](ops/README.md) | Operator diagnostics and tuning: per-subsystem memory ownership, allocator truth, RSS, periodic reporting, and allocator guidance. |
| [solutions/](solutions/README.md) | Shared solution components: deterministic experimental assignment, protected holdouts, treatment/exploration cells, exemptions, and uplift-training records. |
| [gui/](gui/README.md) | The headed app: the Orama design system (read before GUI work), every panel, and the load-bearing rendering notes. |
| [use-cases/](use-cases/README.md) | The use-case catalog framing + UC-1 churn end to end: dataset, entity resolution, the real step executor, the playbook, the at-scale tests. |
| [testing/](testing/README.md) | The in-app test suite, the sim-Python example-script harness, and per-test runbooks. |
| [plans/](plans/README.md) | Forward-looking work plans (current state + next steps), distinct from shipped-slice docs. |
| [research/](research/) | Long-form background material (not descriptions of shipped code): coroutine runtime design, per-cloud CLI compute guides. |
| [reports/](reports/README.md) | Generated deliverables: test reports, benchmarks, audits, measured claims. Dated, and linked from the issue that asked for them. |
| [releases/](releases/README.md) | Cutting or reading a tagged release — what changed, what breaks, what is knowingly still open |

## Conventions for this tree

- **One page per shipped slice**, filed under its domain; long-form background
  goes in `research/` with a provenance header (source + date + status).
- **Every domain directory has a `README.md`** — add your new page to its index
  in the same commit (and to this table only when adding a whole domain).
- Cross-domain links are relative (`../ddf/ddf.md`); cross-repo references are
  full URLs.
- [architecture/code-map.md](architecture/code-map.md) is the file-by-file map —
  update it when files are added, moved, or renamed.
