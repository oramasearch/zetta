# Full context — the reload map

**Purpose:** when someone says *"let's reload the full project context"*, read
this file — it maps everything: what the project is, where every kind of truth
lives, what has shipped, what is in flight, and the working agreements.
**Keep it current**: whenever a PR merges, a plan changes, a wave ships, or a
new foundation lands, update this map in the same commit. A stale map is
worse than none. Last full refresh: **2026-07-02 (post-merge)**.

## The project in one paragraph

**Orama Zetta** turns a question into running, deployed intelligence —
*Intelligence Velocity for Big Thinkers.* `zetta` (this repo) is the product:
a C++26 equal-peer CRDT-replicated cluster with a native distributed data
plane (DDF), native ML (GBDT/TreeSHAP/calibration), a correctness gate,
governance, provisioners for six clouds, and a headed SDL3+ImGui desktop that
is itself a cluster member. The AI **project** is the central item; **use
cases** (UC-1 churn first) are generic AI-project types; the product is
driven **programmatically first** (MCP/REST) and **watched visually** (GUI).

## Repos + systems of record

| What | Where |
|---|---|
| The product (this repo) | `oramasearch/zetta` — branch flow: feature → `dev` → `main` |
| Blueprint (design source of truth) | wtm project `ORAMA-ZETTA` (id 9, ~328 artifacts, 14 domains); projected to GitHub issues + the docs site by `zetta-sync` (skill in amaro-bridge; generators in `zetta-docs/.sync/`); local snapshot: zetta-bridge `docs/blueprint/` |
| Docs website | `oramasearch/zetta-docs` (MkDocs, Railway) — generated, never hand-edit |
| Grouping repo | `oramasearch/zetta-bridge` (submodules: zetta, zetta-docs) |
| Business context | `oramasearch/amaro-bridge` → [orama-platform-2](https://github.com/oramasearch/amaro-bridge/tree/main/docs/projects/orama-platform-2) (one-pager, vision, personas, briefs, design-partner research), [motive-problem-definition](https://github.com/oramasearch/amaro-bridge/tree/main/docs/projects/motive-problem-definition), meetings (2026-07-01 Charles: direction validated, beta in ~2–3 weeks, RL deferred to a seed) |
| Marketing site (public voice) | `oramasearch/amaro-marketingsite`, branch `live` — Orama Zetta rebrand, "Auto-AI for the enterprise" |
| Pattern references | worktree-manager `wtm-mcp` (full-parity MCP); `cppalliance/beast2` (HTTP layer — docs/research/beast2-http.md); installer sidecar (local search API, superseded by distributed-first) |

## Where each kind of truth lives (this repo)

- **Rules + routing**: [CLAUDE.md](../CLAUDE.md) → directory rule-cards
  (src/core · src/gui · include/zetta · tests · docs · schemas) → domain
  READMEs → pages. Navigation: [architecture/code-map.md](architecture/code-map.md).
- **Product**: [product/](product/README.md) — orama-zetta (read-first) ·
  vision · personas · positioning · catalog · control-surfaces ·
  [experience/](product/experience/README.md) (**UX · DX · AX · serving**
  contracts; surface changes update the affected page same-commit).
- **Foundations (conventions)**:
  [architecture/coding-conventions.md](architecture/coding-conventions.md) —
  namespaces==directories, no-throw boundaries, schema-as-code, interfaces;
  **coroutines-first**, **observability-first**
  ([architecture/observability.md](architecture/observability.md): intent ·
  plan · performance · result shape on every operation), **distributed-first**
  (cluster level + machine level, elastic via provisioners).
- **The API surface**: [api/registry-design.md](api/registry-design.md) (one
  registry → MCP/REST/palette; main-thread bridge; auth/consent; job handles;
  Beast2 behind one adapter TU) + [api/adding-a-capability.md](api/adding-a-capability.md).
- **Testing**: [testing/](testing/README.md) — the in-app testkit (ALL cluster
  functionality) + [testing/mcp-journeys.md](testing/mcp-journeys.md)
  (simulated user tests; the promotion rule).
- **Use cases**: [use-cases/](use-cases/README.md) — UC-1 end to end +
  [creating-a-use-case.md](use-cases/creating-a-use-case.md) (compose-don't-
  invent; name the FedEx test).
- **Plans (in-flight)**: [plans/](plans/README.md) — mcp-service ·
  cloud-provisioning.
- **Skills** (`.claude/skills/`): zetta-dev · gui-dev · api-dev ·
  mcp-journeys · add-use-case · github-ops · new-input · perplexity ·
  skill-writer.

## Current state (refresh me when it changes)

- **Workspace solution integration (#1128)** — `deploy/workspace` combines the
  solution foundations with the bus/engine fixes and #1131's in-flight signal
  handler protection. Solution monitors, experiments/assignments and ledger
  additions participate in #1086's incremental reconciliation; policies,
  decisions and ROI definitions remain direct CRDT reads. The audit ID index
  and owner wake-up callback are retained. See
  [cluster/solution-reconciliation.md](cluster/solution-reconciliation.md).

- **Solution monitoring (#1106)** — a replicated monitor per solution records
  input and score PSI, calibration, closed-window cohort performance, and
  declared guardrails. New breach transitions publish to
  `topic:solution-events`; configured monitors also set a durable retrain
  request. See [solutions/monitor.md](solutions/monitor.md).

- **Point-in-time solution foundation (#1103)** — `snapshot` materializes
  replicated `(entity_id, as_of)` tables, the public row-level as-of join picks
  only snapshots visible at each decision time, and `snapshot_lineage` blocks
  live feature sources, future cutoffs, and feature/label windows that violate
  their configured blackout. See
  [solutions/point-in-time-snapshots.md](solutions/point-in-time-snapshots.md).

- **Methodology registry (#1061)** — one 21-entry table drives discovery,
  intent compilation, and run dispatch. All 21 methodologies are runnable on the
  methodology integration branch (#1082), with all method-specific checks,
  runners and reports retained. S-11 resolves to M-8; M-6 remains selectable
  by methodology ID and model kind. M-17 through M-21 add Bayesian uncertainty,
  stacking, label efficiency, privacy and streaming. Unknown model kinds remain
  explicit refusals. All 21 callbacks enforce the
  [data-first feature plan](data/featurization.md) and report actual used/excluded
  inputs with reasons; transform-first data is refused before execution (#1062).
  See [ml/integration.md](ml/integration.md).
  [ml/methodologies.md](ml/methodologies.md) defines the extension contract and
  [ml/README.md](ml/README.md#methodology-portfolio) contains the generated table.

- **Decision-policy engine (#1104)** — native versioned policy config between
  scores and actions, with thresholds, tiers, budgets, caps, eligibility,
  exclusions, A0–A3 approval behavior, and a kill-switch. Policies and decisions
  replicate through the node catalog; all decisions retain their exact policy
  version. Five registry capabilities expose single-entity and table decisions,
  bounded tuning, and A1 approval over MCP and REST. See
  [solutions/decision-policy.md](solutions/decision-policy.md).

- **In review (PR [#1035](https://github.com/oramasearch/zetta/pull/1035), stacked on the
  design PR [#1023](https://github.com/oramasearch/zetta/pull/1023)) — pub/sub stage 1**
  (epic [#1024](https://github.com/oramasearch/zetta/issues/1024)): an in-memory,
  addressed, governed pub/sub in `zetta::pubsub`, owned by the node. Design and decisions:
  [plans/pubsub/](plans/pubsub/README.md); what is built: [pubsub/](pubsub/README.md). One
  node only (the cluster is stage 2, designed later); locks and condition variables by
  decision D1/D5 (the one exception to the coroutine rule).
  Issue #1136 adds node-local topic/message CRUD with revision checking,
  hash-only mutation audit and non-consuming browse over REST/MCP async long
  polls; [pubsub/crud.md](pubsub/crud.md). D39 still defers clustered pub/sub.

- **In flight (branch `feat/dataset-from-connector-query`) — Parquet bodies, partition-only
  datasets, scatter-gather SQL**: [#833](https://github.com/oramasearch/zetta/issues/833)
  makes a connector result set a **cache the cluster holds on disk** rather than rows in the
  document. Landed so far: a native Parquet **writer** (Thrift compact emitter, PLAIN pages,
  Snappy) and `ParquetStore`/`ParquetLazyPtr`, a disk-backed paging `StorageEngine` whose row
  handle survives the cache reaping its page ([data/parquet-writer.md](data/parquet-writer.md),
  [data/parquet-store.md](data/parquet-store.md)); **partition bodies written as Parquet for
  every dataset**, format sniffed from the bytes so legacy `.jsonl` bodies still read
  ([data/partition-body.md](data/partition-body.md)); **partition-only tables**, whose rows are
  never captured into the CRDT — the bodies plus the replicate-to-all catalog are the dataset,
  and `system.datasets` counts from the catalog's row ranges
  ([data/partition-only-tables.md](data/partition-only-tables.md)); **scatter-gather `SELECT`**
  over them — one reachable owner per partition (exactly-once at RF>1), `WHERE` + projection
  pushed near the data, aggregates merged (`avg` weighted by row count), an unreachable owner
  **fails** the query ([data/scatter-gather-select.md](data/scatter-gather-select.md)); the
  durable replication factor raised to **3, clamped to the node count** with connector result
  sets explicitly at **1**; and **`promote_dataset`**, which raises a cache to the durable
  factor by adding owners to the catalog and letting the bodies follow
  ([data/promote-dataset.md](data/promote-dataset.md)); **fetch-affinity placement**, a
  `Partitioner` whose owner is the node that pulled the slice, so a distributed ingest never
  shuffles what it just wrote ([data/fetch-affinity-placement.md](data/fetch-affinity-placement.md));
  the **cross-node co-launch** of the fetch — one node submits, the roster co-launches over
  `run.launch`, every participant pulls ONLY its own tile straight from the source, landing it
  as partition-only partitions it owns, with one `allreduce` making the ingest all-or-nothing
  ([data/distributed-connector-ingest.md](data/distributed-connector-ingest.md)); and the 202
  poll turned into a `runtime::delay` **timer** instead of a `sleep_for` (#832). All ten
  sections of the issue are now implemented.

  Since then, three more: the **partition manifest**, a table-level `mfst:<table-id>` record of
  how many partitions a dataset HAS, so a reader can tell "I can see 6 of 8" from "there are 6"
  and **fail** the query instead of answering a plausible fraction
  ([#837](https://github.com/oramasearch/zetta/issues/837),
  [data/partition-manifest.md](data/partition-manifest.md)) — which in turn surfaced a
  **projection bug in `Node::tick`**: it recorded the CRDT version read at the *end* of the
  tick as reflected, claiming credit for a peer merge the reconcile never saw, so the
  projection stalled permanently short of the document (the root cause of #837's flake). And
  the **partition scheduler**, which replaces `partition_count = 8` with a policy above
  `Partitioner`: a 64 MiB target body size with a floor of partitions per node, capacity-aware
  placement from live telemetry, and a `SourceLayout` that says whether a source's units are
  flexible (fetch straight into the target shape — **no rebalance**) or fixed (fetch
  source-shaped, then rebalance). Because a capacity-aware plan is not derivable per node, the
  coordinator now **computes the plan once and ships it** in the launch payload, so
  derive-don't-transmit is no longer a property of this path
  ([data/partition-scheduler.md](data/partition-scheduler.md)). Known residue, worth its own issue: a `remove`
  (a dropped `part:`/`lo:` key) can be undone by a peer's gossip merge, so a failed ingest can
  leave orphaned catalog keys behind a table that no longer exists — a replication-layer
  delete-versus-merge property, untested anywhere in the tree today.
- **Shipped (this branch) — reachability-aware dialer + unified full-membership `system.nodes`**:
  [#803](https://github.com/oramasearch/zetta/issues/803) makes dial-out-only nodes
  first-class — a node with no routable address (behind NAT / a private VNet: a home
  node, an AKS pod on a private IP) starts with **`--dial-out-only`**, advertises an
  empty dialable set (`advertised_endpoints()` → `{}`; HELLO endpoints `-` + a
  `dial-out` reachability token), and is a pure initiator whose one outbound link
  carries CRDT sync both ways; the dialer **never** dials it (skipping both HELLO-marked
  peers and replicated `node:<id>` entries whose `reachability` is `dial-out-only`),
  fixing the cross-cluster "7 not 9" undercount when two k8s clusters share a pod CIDR
  (`10.244.0.x`). The node-list entry is now `node:<id> → { address, reachability, node_type }`
  (`Cluster::primary_advertised_address()` computes the dialable address, empty when
  dial-out-only; `node_type` is the self-declared durability class `permanent` | `ephemeral`,
  set with the new **`--node-type`** flag — RF is maintained over the permanent set, a separate
  concern). The full-membership view is now folded into a single unified **`system.nodes`**
  (#809): the old `system.cluster_nodes` is gone, and `system.nodes` is a **virtual table over
  the node-list CRDT** — every `node:<id>` the document knows (all N from any node, no "5 not 9"
  undercount), composed at query time from a durable tier (`node_id`, `address`, `reachability`,
  `node_type`) overlaid with ephemeral, never-written-back tiers: local liveness (`is_self`,
  `directly_connected`, `status` live|unreachable, `last_seen_seconds`) and synced replication
  status (`entries`, `version`, `in_sync`, `last_sync_seconds`, for self + direct peers). The
  full-membership source (#800) and reachability-gated dialer (#803) still underpin it. See
  [cluster/cluster.md](cluster/cluster.md) and [data/query.md](data/query.md). **Slice 3 (#809)**
  generalizes the primitive into a user-facing DDL: **`CREATE VIRTUAL TABLE <name> OVER CRDT
  '<prefix>' (col type TIER…)`** declares a read-only virtual table over *any* CRDT key-family
  (durable KEY + durable/ephemeral columns), stored as durable `vtable:<name>` catalog metadata
  and projected at query time. Ephemeral-column *resolvers* (`FROM <source>`) are the documented
  follow-up. See [data/virtual-tables.md](data/virtual-tables.md).
- **In flight 2026-07-03 (governed serving)**: [#648](https://github.com/oramasearch/zetta/issues/648)
  — models and indexes exposed as governed HTTP endpoints: `expose_model` /
  `expose_index` flip a replicated `exposed` flag on the entity value, the
  Beast2 adapter serves `POST /v1/models/{name}/predict` (batch scoring) and
  `GET /v1/indexes/{name}/search` (the #641 scatter-gather) only while the
  flag is set; endpoint access is key-scoped to `data:predict` /
  `data:search`, every request a `serve:*` span + audited; `exposed` +
  `endpoint` in `system.models` / `system.indexes`
  ([docs/api/serving.md](api/serving.md)).
- **Shipped 2026-07-03 (remote consent)**: [#621](https://github.com/oramasearch/zetta/issues/621)
  — the consent challenge protocol for cloud-touching MCP tools: a
  `requires_consent` capability never executes on first call (structured
  challenge naming exactly what would be read/run), approval only by a
  distinctly-privileged principal (approver key of a different principal, or
  the Admin panel's headed approval surface), executed exactly once, audited
  + span'd on both sides; `ConsentLedger` on the Node,
  `system.consent_requests`, the consent-gated `cloud_inspect` /
  `detect_provisioners` / `add_nodes` tools, and `ProvisionerFleet` moved to
  core so headless MCP adds drive the same wired provisioners
  ([docs/api/consent.md](api/consent.md)).
- **Merged to `dev` 2026-07-02**: the full restructure + foundations chain —
  [#610](https://github.com/oramasearch/zetta/pull/610) docs domain
  restructure, [#649](https://github.com/oramasearch/zetta/pull/649)
  (supersedes #612) code/tests/namespaces match domains,
  [#613](https://github.com/oramasearch/zetta/pull/613) control-surfaces
  product docs, [#644](https://github.com/oramasearch/zetta/pull/644)
  foundations (API registry + observability core + coroutine-first bridge,
  three conventions, experience contracts, product build-out, four skills;
  11/11 tests). `dev` promoted to `main` immediately after.
- **Tests**: the fulltext ranking-oracle pair
  ([#611](https://github.com/oramasearch/zetta/issues/611)) is fixed in
  [PR #660](https://github.com/oramasearch/zetta/pull/660) — a
  statistics-engineered gtest fixture + the local-IDF query-then-fetch
  contract in the testkit case; the exact general fix (global-statistics
  pass) is [#657](https://github.com/oramasearch/zetta/issues/657). Two
  ml-domain failures remain pre-existing on clean `dev` locally
  ([#664](https://github.com/oramasearch/zetta/issues/664), GBDT near-data
  histogram-allreduce).
- **Merged to `dev` 2026-07-02 (search)**: [#641](https://github.com/oramasearch/zetta/issues/641)
  — THE canonical search entry point: `Node::search` routes distributed-first
  (the node-owned `DistributedFulltextIndex`, sub-indexes reconciled from
  tick) with per-node provenance + one trace; surfaced as the `search` MCP
  capability and the GUI Search view
  ([docs/indexing/node-search.md](indexing/node-search.md)).
- **Merged to `dev` 2026-07-02 (search view)**: [#643](https://github.com/oramasearch/zetta/issues/643)
  — the Search view finished as a first-class distributed+observable surface:
  sub-indexes-queried provenance end to end (`NodeContribution.sub_indexes`,
  the `fulltext_hits` wire field, the `search` capability), the provenance
  strip + the cluster strip replaying the scatter-gather as #590 flows,
  master-detail hits with source-row fields and entity_links
  ([docs/gui/search-view.md](gui/search-view.md)).
- **Merged to `dev` (search capability rounded out)**: [#642](https://github.com/oramasearch/zetta/issues/642)
  — `list_indexes` (the index registry as a tool — discover what `search` can
  target) and the layer-1 distributed search journey (3-node loopback
  cluster: list_indexes → search → ranked hits + per-node provenance →
  get_trace per-peer spans; `tests/api/test_search_journey.cpp`).
- **Merged to `dev` 2026-07-03 (action parity)**: [#617](https://github.com/oramasearch/zetta/issues/617)
  — governed action parity: the mutating product loop over MCP
  (capture_intent · import_n8n · compile_intent/n8n/python · gate · deploy ·
  run · add_dataset · testsuite_run), each `orchestration:<name>`-scoped +
  audited under the acting key; `run`/`testsuite_run` are kind:job over the
  engine's durable Run handles (`api::BackgroundActions` owns the workers,
  `testkit::AsyncCaseRun` is the new testkit entry point)
  ([docs/api/governed-actions.md](api/governed-actions.md)).
- **Merged to `dev` 2026-07-03 (PR #709)**: [#620](https://github.com/oramasearch/zetta/issues/620)
  — GUI live surfacing of externally-initiated actions: the registry's
  activity hook wired into the headed app (thread-safe queue drained in the
  frame loop), surfaced as a click-through "MCP · action · actor" strip line
  deep-linking via `navigate_to`; palette self-calls suppressed; refresh +
  animations needed zero wiring (live reads each frame)
  ([docs/gui/external-activity.md](gui/external-activity.md)).
- **Merged to `dev` 2026-07-03 (PR #711)**: [#619](https://github.com/oramasearch/zetta/issues/619)
  — journeys as DATA: the shared `JourneyStep{tool, input, expect}` driver
  (`tests/api/journey.hpp`) + the three canonical journey tables
  (`tests/api/journeys_canonical.hpp` — UC-1 happy path with the near-data
  trace read-back, gate-blocked deploy, dataset ingest + placement), run at
  layer 1 (`test_action_journeys.cpp`) and replayed unchanged over real
  JSON-RPC at layer 2 (`test_mcp_server.cpp`); failures name the offending
  step + assertion ([testing/mcp-journeys.md](testing/mcp-journeys.md)).
- **In flight (this branch)**: [#618](https://github.com/oramasearch/zetta/issues/618)
  — the REST rendering + OpenAPI generation: the registry as plain HTTP
  (reads → `GET /v1/<name>` with schema-coerced query input, actions/jobs →
  `POST /v1/<name>`, same bearer → AccessKey auth + audit, error codes →
  HTTP statuses), `Capability.output_schema` on all 35 entries (entity reads
  compose theirs from the entity `*_value_schema()`; also rendered as the MCP
  `outputSchema`), and `GET /v1/openapi.json` generated from the registry —
  drift-checked into `schemas/openapi.json`
  (`ZETTA_WRITE_SCHEMAS=1 ctest -R OpenApi`); loopback-HTTP journeys in
  `tests/api/test_rest_server.cpp` + generator/drift tests in
  `tests/api/test_openapi.cpp` ([docs/api/rest-openapi.md](api/rest-openapi.md)).
- **Backlog**: triaged 2026-07-02 — 51 tracker issues closed with
  explanatory comments (shipped / duplicates / risk-register); all open
  issues are actionable.
- **The wave**: umbrella [#639](https://github.com/oramasearch/zetta/issues/639);
  epics [#614](https://github.com/oramasearch/zetta/issues/614) (MCP/REST) +
  [#627](https://github.com/oramasearch/zetta/issues/627) (GUI usability);
  **wtm plan 19** ("Product wave") — 36 nodes / 32 agent-analyzed edges
  (don't run the wtm GUI generate buttons on it; they overwrite the analysis).
  **Both foundations are MERGED**: #615 MCP server (PR #653 — Beast2-embedded
  POST /mcp in both run modes) and the GUI foundation (PR #655 — per-view TUs
  #638 closed · tones + widget grammar (#630/#636 cores) · view registry +
  navigate_to + sidebar, #628 closed). Plan 19 reconciled 2026-07-03: grammar
  edges inverted (it shipped, so #634 master-detail leads the panel sweeps,
  #631/#632/#633 follow it, #630 audit + #635 terminology close); layer-0 now
  #637 · #611 · #466 · #538 · #641 · #634 · #626;
  #616/#625/#645 also unblocked. **#636 adoption swept**: every panel status
  renders via tone_of + status_label/status_pill
  ([status-tones.md](gui/status-tones.md)). **#632 feedback grammar swept**:
  every mutating affordance reports through the per-panel `banner` (engine
  reason verbatim, silent failures surfaced, async outcomes land as banners)
  ([feedback-grammar.md](gui/feedback-grammar.md)). **#622 activity feed shipped**
  from layer-0: `system.activity` + the Operations → Activity view + the Home
  recent-activity strip ([docs/gui/activity-feed.md](gui/activity-feed.md)).
  **#625 command palette shipped**: Cmd+K fuzzy view navigation + entity jump
  + the capability registry's actions rendered for humans, executed through
  the one `Registry::call` path
  ([docs/gui/command-palette.md](gui/command-palette.md)).
  **#633 cross-entity links wired**: `entity_link`/`link_line` on every
  cross-entity reference (Run ↔ Model ↔ Job, Run → Graph → Project,
  Deployment → Run, Dataset → Cluster, Audit → target); the Graph workspace
  and Deploy panels now consume their pending selections
  ([docs/gui/cross-entity-links.md](gui/cross-entity-links.md)).
  **#629 Home is a real dashboard (SCR-1)**: cluster health strip +
  needs-attention + recent runs + projects with last activity + the
  intent→deployed velocity KPI, every tile a click-through
  ([docs/gui/home-dashboard.md](gui/home-dashboard.md)).
  **#623 first-run onboarding shipped**: the empty Home's guided demo card —
  five steps from a fresh data_dir to an explained churn result (demo data →
  pre-filled project → gate + deploy → cluster run → at-risk + drivers), each
  naming its panel, replayable from Help · Guided demo; new core surface
  `write_churn_dataset` (the loader's schema-validated inverse)
  ([docs/gui/onboarding.md](gui/onboarding.md)).
  **#635 product-language pass swept**: every view audited, kind/status enums
  render via `label_of`/`humanize_kind` in the one mapping site
  `src/gui/labels.hpp` (SQL keeps raw wire values), `term_hint` hovers name
  unavoidable technical terms, per-view sign-off recorded
  ([docs/gui/copy-audit.md](gui/copy-audit.md)).
- **Security**: CTL-2/TB-3 inter-node transport security is **built**
  ([#466](https://github.com/oramasearch/zetta/issues/466),
  [docs/cluster/security.md](cluster/security.md)): mutual TLS 1.3 on every
  cluster link, per-node Ed25519 identities, token-gated join (the token
  derives the cluster CA), **always on** since #799 — which removed the
  `--cluster-plaintext` opt-out (no plaintext peer mode) and made the default-deny
  HTTP/MCP auth gate non-optional (there is no auth flag or toggle — the
  `--require-auth` flag and `ZETTA_REQUIRE_AUTH` env were removed, and the built-in
  `admin` role gained the orchestration atoms so the owner can drive orchestration
  over HTTP/MCP; a never-locked-out node auto-seeds an owner token when no
  `ZETTA_ADMIN_TOKEN` is supplied). PR [#667](https://github.com/oramasearch/zetta/pull/667); follow-up
  still open: [#669](https://github.com/oramasearch/zetta/issues/669) CSR
  enrollment / peer-cert pinning / revocation.
  [#670](https://github.com/oramasearch/zetta/issues/670) (provisioner
  join-token plumbing) is **done** — burst nodes now get `ZETTA_CLUSTER_TOKEN`
  alongside `ZETTA_SEED_PEER`, so a provisioned node joins the secure cluster
  (PR [#742](https://github.com/oramasearch/zetta/pull/742)).
- **Churn-run consolidation shipped
  ([#637](https://github.com/oramasearch/zetta/issues/637), PR
  [#661](https://github.com/oramasearch/zetta/pull/661))**: ONE public churn
  runner — `run_churn_solution_on_node` over the node-owned `JobCoordinator`
  (#579 spine); GUI demo (loopback fallback removed), console graph runner,
  testkit UC-1 cases, and the acceptance tests all route through it. Unblocks
  the MCP `run` tool (#617) and the SPMD co-launch (#601); harness-dedup tail
  filed as [#681](https://github.com/oramasearch/zetta/issues/681).
- **Coordinated cross-process SPMD launch shipped
  ([#601](https://github.com/oramasearch/zetta/issues/601))**: the co-launch
  half of distributed compute — a schema-validated `run.launch` Task (the
  first consumer of `SignalKind::Task`) fans an interactive run out to the
  live membership; each member's `CoordinatedLaunchListener` (the
  coordinator's signal fallback, drained by `Node::tick`) co-runs the
  registered kind over its own disjoint entity shard on its owner thread.
  Churn is the first kind (`run_churn_solution_on_node` now fans out on a
  multi-node cluster; peers re-gate, RULE-1 everywhere); the
  `ChurnStepExecutor` slices the broadcast feature table to local entities
  (the latent multi-node double-count fixed). Proven single-node ==
  multi-node + one root Model replicated everywhere over a real replicated
  3-member loopback cluster
  ([docs/compute/coordinated-launch.md](compute/coordinated-launch.md)).
  Near-data (#531) cross-process launch on this boundary is the follow-on.
- **Observability build-out (#640, closed) — PR #665 in flight**: all six
  slices on one branch — system.traces/spans/crashes read views over the
  Node-owned span ring (`Node::span_recorder()`, RULE-7 one source), crash
  reports (`observability/crash.hpp`, fatal-signal handler →
  `<data_dir>/crashes/<uuid>.json`, surfaced on start), cross-node trace
  propagation (replicated `trace_id` on Run/ComputeJob; traceparent on the
  signal envelope + steal offers; job/round/map/steal/near-data/run/step
  spans), traced testkit (`testkit:<case.id>` spans), the GUI Traces view
  (the per-operation trace pane beside #668's Activity feed), and the
  `submit_feedback` capability (bundle as a large object, explicit export).
- **#624 shipped (long-running actions off the UI thread)**: the Node
  **owner-thread gate** (`Node::run_on_owner`, drained in `tick()`;
  [architecture/owner-gate.md](architecture/owner-gate.md)) lets background
  workers drive engine runs safely — the churn run (`AsyncChurnRun`, Deploy
  panel + Graph Workspace, cooperative between-steps cancel), dataset add
  (`AsyncDatasetAdd`, worker decode + pump-sliced materialize + cancel), and
  the consent-approved cloud inspect (spawn/poll) all keep the UI interactive
  with live progress from the same records MCP polls (system.runs /
  system.jobs). New audit actions `cloud_inspected` + `dataset_added` put
  their completions in the activity feed
  ([gui/long-running-actions.md](gui/long-running-actions.md)).
- **#530 shipped** (UC-1 featurize → train over partitions):
  `zetta::ml::run_near_data_featurize_train_job` composes #532 featurize +
  #533 train so the GBDT trains on the feature table, and `cluster.uc1_at_scale`
  now featurizes before training — see
  [use-cases/uc1-featurize-train-over-partitions.md](use-cases/uc1-featurize-train-over-partitions.md).
- **TC-12 composed (#502): the cost-capped burst lifecycle.** The sim-Python
  `cluster.add_nodes` gained a `cost_cap=` kwarg; the Node-aware
  `run_add_nodes_step` wraps the injected provisioner in the `BurstApprovalGate`
  (over-cap blocked deny-by-default + audited) and rebalances partitions onto
  the joined members. `cluster.uc1_at_scale` now runs the whole UC-4 lifecycle
  over a **replicated** loopback cluster (the testkit harness's opt-in
  replicated mode): one partitioned saas table across the permanent members →
  gated burst → rebalance on (bodies follow) → burst identities minted → SPMD
  featurize→train→explain→rank across the grown cluster → teardown (partitions
  drain back, identities revoked, model still serving) — see
  [use-cases/uc1-at-scale-e2e.md](use-cases/uc1-at-scale-e2e.md). Still later:
  the real Azure run (needs the
  [cluster-join slice](plans/cloud-provisioning.md)) and the real HF-hub
  dataset.
- **#645 llms.txt endpoints built**: the MCP server also answers plain
  `GET /llms.txt` / `/llms/<topic>.txt` / `/llms-full.txt` — agent docs
  generated live from the registry + node state, no second prose source
  ([api/llms-endpoints.md](api/llms-endpoints.md)).
- **#646 spec index built** (and the #469 fold completed with it):
  `GET /v1/specs` + `/v1/specs/<name>` and the `list_specs`/`get_spec` tools
  serve the OpenAPI document + every capability schema + all 86 boundary
  schemas from their C++ accessors; the shared table is
  `zetta::schemas::boundary_schemas()`, drift-checked by `test_schemas`
  ([api/specs-index.md](api/specs-index.md)).
- **Issue clusters**: MCP #615–#621 · GUI features #622–#626 · GUI design
  #628–#636 · prerequisites #637/#638 · observability #640 · distributed
  search #641–#643 · AX #645 llms.txt / #646 spec index · web #647 (next
  wave) · serving #648. Deferred by decision: #138 tiered storage (own wave),
  #467 sandbox (+CTL-3/4, extension milestone), FEAT-11/12/13 RL trio.

## Working agreements

- Feature branches, PRs target `dev`; never push `main`/`dev`. Conventional
  Commits; **never** mention AI/Claude in commits/PRs/issues.
- Docs land in the same commit as the change, at every disclosure level
  (page + domain README + code-map + rule-card + this map when state shifts).
- Cloud access is consent-gated, per-action, deny-by-default — for the
  product AND for agents working on it.
- wtm blueprint: sync reads wtm, writes issues/docs — never write to wtm from
  the sync. Plan edges are agent-analyzed.
- The repo memory system is git: no agent memory store; durable knowledge
  goes in CLAUDE.md / docs / skills.
