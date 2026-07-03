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
  namespaces==directories, no-throw boundaries, schema-as-code, seams;
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
- **In flight (this branch)**: [#643](https://github.com/oramasearch/zetta/issues/643)
  — the Search view finished as a first-class distributed+observable surface:
  sub-indexes-queried provenance end to end (`NodeContribution.sub_indexes`,
  the `fulltext_hits` wire field, the `search` capability), the provenance
  strip + the cluster strip replaying the scatter-gather as #590 flows,
  master-detail hits with source-row fields and entity_links
  ([docs/gui/search-view.md](gui/search-view.md)).
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
- **Security**: CTL-2/TB-3 inter-node transport security is **built**
  ([#466](https://github.com/oramasearch/zetta/issues/466),
  [docs/cluster/security.md](cluster/security.md)): mutual TLS 1.3 on every
  cluster link, per-node Ed25519 identities, token-gated join (the token
  derives the cluster CA), secure by default with `--cluster-plaintext` the
  opt-out. PR [#667](https://github.com/oramasearch/zetta/pull/667); follow-ups
  filed: [#669](https://github.com/oramasearch/zetta/issues/669) CSR enrollment
  / peer-cert pinning / revocation,
  [#670](https://github.com/oramasearch/zetta/issues/670) provisioner
  join-token plumbing.
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
  Near-data (#531) cross-process launch on this seam is the follow-on.
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
- **#530 shipped** (UC-1 featurize → train over partitions):
  `zetta::ml::run_near_data_featurize_train_job` composes #532 featurize +
  #533 train so the GBDT trains on the feature table, and `cluster.uc1_at_scale`
  now featurizes before training — see
  [use-cases/uc1-featurize-train-over-partitions.md](use-cases/uc1-featurize-train-over-partitions.md).
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
