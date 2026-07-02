# Full context — the reload map

**Purpose:** when someone says *"let's reload the full project context"*, read
this file — it maps everything: what the project is, where every kind of truth
lives, what has shipped, what is in flight, and the working agreements.
**Keep it current**: whenever a PR merges, a plan changes, a wave ships, or a
new foundation lands, update this map in the same commit. A stale map is
worse than none. Last full refresh: **2026-07-02**.

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

- **PR chain (stacked, retarget to `dev` as each merges)**:
  [#610](https://github.com/oramasearch/zetta/pull/610) docs domain
  restructure → [#612](https://github.com/oramasearch/zetta/pull/612)
  code/tests/namespaces match domains (full hard rename) →
  [#613](https://github.com/oramasearch/zetta/pull/613) control-surfaces
  product docs → [#644](https://github.com/oramasearch/zetta/pull/644)
  foundations (API registry + observability core + bridge, three conventions,
  experience contracts, product build-out, four skills; 11/11 tests).
- **Tests**: 1077/1079 green; the 2 failures are pre-existing on clean `dev`
  ([#611](https://github.com/oramasearch/zetta/issues/611), fulltext
  ranking-oracle, reproduced + analyzed).
- **Backlog**: triaged 2026-07-02 — 51 tracker issues closed with
  explanatory comments (shipped / duplicates / risk-register); all open
  issues are actionable.
- **The wave**: umbrella [#639](https://github.com/oramasearch/zetta/issues/639);
  epics [#614](https://github.com/oramasearch/zetta/issues/614) (MCP/REST) +
  [#627](https://github.com/oramasearch/zetta/issues/627) (GUI usability);
  **wtm plan 19** ("Product wave") — 36 nodes / 31 agent-analyzed edges
  (don't run the wtm GUI generate buttons on it; they overwrite the analysis).
  Layer-0 unlocks: #615 MCP server (core shipped; Beast2 adapter remains),
  #637 churn-runner consolidation, #638 app_operations TU split.
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
