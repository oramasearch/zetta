# Codebase audit — inconsistencies, redundancies, smells + blueprint reconciliation

A point-in-time health audit of the zetta codebase: where it drifts from its own
stated conventions, where it duplicates itself, the structural smells worth paying
down, and how it lines up against the **wtm `ORAMA-ZETTA` blueprint** (epic
[#279](https://github.com/oramasearch/zetta/issues/279); worktree-manager project
`ORAMA-ZETTA`, 300 artifacts). Produced 2026-06-27 by three grounded read-only
audit passes (redundancy / inconsistency / code-smell) plus a blueprint-rule
reconciliation.

This is a living document — keep it current as items are paid down, and add the
blueprint rules here when they change so the codebase has one place that says
"these are the rules and here is how we measure against them."

> **Decisions taken** (recorded inline below): schema source-of-truth moves into
> the C++ types (a `schema()` method generates the JSON; a build step keeps
> `schemas/*.json` in sync); add the four missing `system.*` tables; collapse
> the churn-pipeline duplication onto one shared encoding header + the churn runner
> (now `run_churn_solution_on_node`, #637);
> keep `testkit/` in `zetta-core` for now.

---

## 1. Blueprint reconciliation — the rules and how we measure up

The blueprint's **business rules (RULE-*)**, **permission rules (PERM-*)**,
**security controls (CTL-*)**, **trust boundaries (TB-*)** and **NFRs** are the
canonical "rules" the codebase should follow. Copied here (condensed) with a
compliance verdict: ✅ built · 🟡 partial / enforced in only one place · ⛔ gap ·
🔭 planned (explicitly a later slice).

| Rule | Statement | Status |
|------|-----------|--------|
| **RULE-1** | No deploy without a passing correctness gate (a graph can't deploy unless its `ValidationReport` passes). | 🟡 The gate exists and the **GUI Deploy panel** runs it first, but `Node::deploy_graph` does **not** run the gate — a programmatic deploy bypasses RULE-1. Wire the gate into the `deploy_graph` seam (or a `deploy_graph` that refuses an ungated graph). |
| **RULE-2 / NFR-1 / CTL-4 / TB-1** | No data egress by default; any outbound network is explicitly configured + audited; verifiable execution locality. | 🔭 The cloud-provider **permission gate** (deny-by-default, ask-every-time) is built for config/CLI reads. A general egress control over graph/extension execution + packet-capture verification is unbuilt (depends on the extension sandbox, TB-2/CTL-1). |
| **RULE-3 / PERM-1 / ADR-6** | Key-scoped access (V1): a principal operates over exactly what its login key permits. | ✅ Built (`Principal`/`AccessKey`/`Scope`/`Capability`, deny-by-default). 🟡 **but** `Principal`/`AccessKey` records (replicated `principal:`/`key:` keys — auth credentials) enter via ad-hoc `contains()` checks, **not** the formal schema boundary their siblings Run/Model/Job/Audit use. See §2.1. |
| **RULE-5 / RSK-9** | Burst jobs must be eviction-tolerant (checkpoint/restart to survive Spot eviction). | 🟡 The **checkpoint/restart capability is built** ([job-checkpoint.md](../compute/job-checkpoint.md), #468): a durable, sha256-fingerprinted `JobCheckpoint` (validated boundary) + a `CheckpointStore` seam (`FileCheckpointStore`, atomic + crash-safe) + `run_resumable_rounds` (resume from the last checkpoint with no recompute, graceful spot-drain), wired into per-round [GBDT](../ml/gbdt.md) model checkpointing + a `system.checkpoints` view. Remaining: a **shared/replicated** store so a *replacement* burst node resumes a distributed job, and **auto-restart** from the autoscaler on a *detected* eviction. |
| **RULE-6 / CTL-3 / ENT-17** | Secrets stored as vault references, never inline in project/run records or logs. | ✅ No inline secrets found — cloud configs reference profiles / managed identities / a credentials-ref, not embedded credentials. (No vault integration yet, but nothing violates the rule.) |
| **RULE-7** | Observable state defaults to a virtual, read-only `system.*` table computed at query time — not a GUI-only panel or hand-synced copy. | 🟡 **Drift.** `system.nodes/_deployments/_models/_jobs/_cloud_providers/_cloud_status` comply, but **Runs, Principals, Keys, and the Audit trail are GUI-only with no table** — though Model and Job (Run's siblings) both have one. The rule itself warns this is exactly how "SQL and the GUI drift." **Decision: add all four.** See §2.2. |
| **PERM-2 / CTL-6** | Only principals holding the orchestration capability may provision infra / deploy nodes. | 🟡 The capability model exists (`CapabilityDomain::Orchestration`); whether `deploy_graph` / the provisioners actually *check* it is not yet wired (they're not behind an `authorize(operation)` call). |
| **CTL-1 / TB-2** | Capability-scoped deny-by-default sandbox: no ambient authority, no fs/network by default, all calls audited (the extension/runner sandbox). | 🔭 Planned — no extension/runner sandbox yet (RSK-7). |
| **CTL-2 / TB-3** | mTLS on all inter-node traffic + cryptographic per-node identity + token-gated join. | 🟡 **Built** ([cluster/security.md](../cluster/security.md), #466): mutual TLS 1.3 on every link (handshake before any protocol line, fail-closed), per-node Ed25519 identity certs, and a token-gated join (the token derives the cluster CA) — on by default in the binary, `--cluster-plaintext` the explicit opt-out. Burst provisioners now inject the join token (`ZETTA_CLUSTER_TOKEN` alongside `ZETTA_SEED_PEER`) so a provisioned node joins the secure cluster (#670). Remaining: member-vs-member node-id impersonation (needs CSR enrollment + a Corosio peer-cert accessor) and token rotation/revocation UX. |
| **CTL-5** | Immutable audit trail + approval gates for risky/shared actions. | ✅ Audit trail built (append-only `AuditEvent`); `BurstApprovalGate` gates over-cap bursts. Extension-publish approval (RULE-4/APR-1) is 🔭. |
| **ADR-17** | No foreign runtime — native reimplementations, no Python/XGBoost/warehouse in the engine/build/CI (RSK-8). | ✅ Followed rigorously; the [native correctness oracle](../ml/native-oracle.md) mitigates the RSK-8 correctness/maintenance burden. |

**Net:** the *data-plane and governance primitives* are largely built and faithful
to the blueprint; the gaps cluster in (a) **enforcement wiring** (RULE-1 not at the
`deploy_graph` seam, PERM-2 not checked), (b) **the security hardening track**
(CTL-2 mTLS now built with follow-ups above; CTL-1 sandbox; RULE-5 eviction
tolerance now has its
[checkpoint/restart primitive](../compute/job-checkpoint.md), with a shared store + auto-restart
trigger remaining), and (c) **one concrete RULE-7 drift** (missing system tables).
(a) and (c) are cheap to close now; (b) is a planned epic.

---

## 2. Inconsistencies (drift from our own conventions)

### 2.1 `Principal` / `AccessKey` skip the formal schema boundary *(security-relevant)*
The replicated `principal:<uuid>` / `key:<uuid>` records are **auth credentials**,
siblings of `Run`/`Model`/`ComputeJob`/`AuditEvent` — all of which validate against a
formal JSON Schema at the storage boundary. Principals/keys instead enter via bare
`contains()` / `is_string()` checks (`src/core/node.cpp` load path,
`src/core/governance/principal.cpp`). `schemas/principal.schema.json` is **orphaned** (referenced
only by a doc, no validator), and `schemas/access_key.schema.json` is enforced only by
hand-mirrored comments. CLAUDE.md never flags this as known debt. **Fix:** route
principal/key loads through the same boundary as their siblings (see §5 schema plan).

### 2.2 RULE-7 — missing `system.*` tables
`Runs`, `Principals`, `AccessKeys`, and the `Audit` trail are observable only through
GUI panels (`draw_runs`, `draw_admin`) — no `system.runs` / `_principals` /
`_keys` / `_audit`, while Model and Job have tables. **Decision: add all four**,
computed at query time from `Node::runs()` / `principals()` / `audit_events()` (the
keys row redacts nothing secret — scope + capabilities only, same as the Admin panel).

### 2.3 Split `from_json` result convention
Within the hand-written boundary family, the **access-control + validation cluster**
returns `bool` + out-param (`scope_from_json`, `capability_from_json`,
`validation_report_from_json`) while ~20 sibling value types
(`NodeCapability`/`ScalingPolicy`/`DataFrameSchema`/the 6 cloud configs/`DeviceInfo`/…)
return `std::optional`. Undocumented micro-split, localized to the least-conformant
corner (access-control). **Fix:** pick `std::optional` for the family and migrate the
three outliers.

### 2.4 On-disk projection validation is uneven
`Run`/`Model` re-validate from disk via the formal schema; `Database`/`Project`/
`ProjectInput`/`Deployment` use only manual `contains()` checks. Defensible (they're
projections of CRDT state) but inconsistent and undocumented.

### 2.5 Naming drift vs the "full descriptive names" rule
- `src/core/compute/cpu_device.cpp` uses `template <class T>` (6×) — the **only**
  single-letter template parameter in the tree, directly against the "`Value`/`Result`
  not `T`" rule.
- Single-letter loop counters (`i`/`j`/`f`/`g`/`r`) persist across ~85 sites
  **including the brand-new `testkit/` files** — the "rename when you touch" rule was
  meant to stop exactly this. `harness.cpp` also has `ChurnAccount a` / `ChurnEvent e`
  / `std::error_code ec` (the rule names `ec` specifically).

> Items the audit confirmed **consistent / not drift**: the no-throw-across-the-seam
> rule (throws are confined + caught at every real seam; the testkit `run_test_case`
> try/catch is the documented zetta-core↔GUI exception firewall, not a violation —
> zetta-core is `-fexperimental-library`, not `-fno-exceptions`); the postgres
> `ParseError` (wrapped); `parallel_for`'s abort-on-throw; CUDA's status conversion;
> `Node::cloud_inspections_` (ephemeral, has its `system` view); the Cluster
> peer-table being outside the CRDT (documented migration debt); the by-id-only
> `ScalingPolicy` reference (documented later slice).

---

## 3. Redundancies

| # | Redundancy | Size | Intent |
|---|-----------|------|--------|
| R1 | **Churn numeric-encoding helpers** (`day_of_year_2026`, `event_type_code`, `encode_churn`, `*_schema`, `churn_*_spec`, `make_store`) copy-pasted across **~10** files (8 test files + `churn_solution.cpp` + `testkit/harness.cpp`). Two copies even disagree on the date encoding (`churn_solution.cpp` uses a general `days_from_civil`; the rest a 2026-only lookup table). | **~700–800 lines** | Accidental / migration debt (the shared home postdates the tests). |
| R2 | **`ComputeNode` single-node harness** (Node + Cluster + started `JobCoordinator`, ~27 lines) duplicated near-verbatim across **~10** test files; `testkit::LoopbackCluster` is the canonical replacement. | **~250 lines** | Accidental. |
| R3 | **JSON Schema dual-maintenance** — every schema lives in BOTH `schemas/*.json` AND (for pboettch entities) an embedded draft-07 string in the `.cpp`; only the embedded copy is enforced, the files are never loaded and **already drift** (annotations differ). `run`/`model`/`n8n_workflow` embed a schema with **no** `.json` file at all. | **41 schemas × 2 places** | Documented pattern, but structurally fragile (silent rot). |
| R4 | **Provisioner orchestration boilerplate** — the ctor + `provision`/`teardown`/`provisioned` are byte-identical (modulo class name) across the 6 backends; plus `read_string`/`read_uint32`/`flag()`/`parse_nodes()` copy-pasted into all 6 + their CLI control planes. | **~375 lines** collapsible | Intentional "mirror" pattern (documented), but the orchestration layer is mechanical. |
| R5 | **`make_store`** rolled independently in **15** test files. | minor | Accidental. |

**Decision on R1:** promote one `churn_numeric_encoding.hpp` (encode + schemas + spec
+ `make_store`) into the churn fixture; have `churn_solution`, the testkit harness, and
every test include it — and standardize on the general `days_from_civil` encoding so
the two variants converge. (Partially done by #637: the testkit harness now reuses the
canonical `churn_solution.hpp` encoding and no longer carries its own `make_store`;
the remaining copies are in `tests/`.)

---

## 4. Code smells

- **`src/gui/app.cpp` is a 3,574-line monolith** (one `App` with ~30 `draw_*` methods
  in one TU). Split into per-panel translation units (`app_graph.cpp`, `app_deploy.cpp`,
  `app_cloud.cpp`, `app_jobs.cpp`, `app_test_suite.cpp`) sharing `app.hpp`; each large
  `draw_*` moves whole.
- **`Node::reconcile_from_replication` is one ~512-line function** (`node.cpp`) that
  repeats an identical `scan → drop-locals-absent-from-doc → materialize-new` block ~11
  times, once per entity type. Extract one `reconcile_<entity>(snapshot)` helper per
  type; the body becomes ~11 calls.
- ~~**Three inline re-wirings of the churn pipeline.**~~ **Resolved (#637):** every
  in-process churn submitter — the GUI's `run_churn_demo`, the governed console's
  churn-shaped path, the testkit UC-1 cases, and the acceptance tests — now routes
  through the one public runner, `run_churn_solution_on_node`, over the node-owned
  `JobCoordinator`; the isolated-coordinator wiring survives only as a file-local
  detail of `churn_solution.cpp`.
- **`testkit/` + `churn_solution.cpp` are compiled into production `zetta-core`** (the
  GUI panel links them). **Decision: keep in `zetta-core` for now** (a CPU-only build
  carries little cost) — revisit if core bloat becomes a concern; the clean exit is a
  dedicated `zetta-testkit` static lib (the way `zetta-compute` is isolated).
- **"GPU-native" is staged but unbuilt** — every ML op (featurize/train/score/explain)
  is a host-side reference over device-backed data; the device-native kernels are marked
  "later slice" everywhere. This is the largest real architectural gap behind the
  product's headline "GPU-native, CPU-fallback, distributed" direction, and deserves a
  tracked epic rather than scattered comments.
- **Other deferred slices** (real gaps, all documented as "later"): single-node-only
  churn solution (multi-node SPMD equivalence only in the distributed tests, not the
  shipped entry); leader election + failover (`ClusterRole` always `Follower`); cluster
  membership not yet in the CRDT; large-object / model-weights / input-payload **body
  transfer** (metadata replicates, bytes don't); deployment records a summary only
  (IR has no serialized form on a deployment); autoscaler / `ScalingPolicy` not wired to
  a live job; CREATE TABLE column types parsed but not stored; k8s/cloud deploy targets
  + Parquet format declared but non-functional.

> **Healthy:** no dead / commented-out code; `(void)param` casts are all legitimate;
> headers are layer-clean (no SDL/ImGui in `zetta-core`, no CUDA in a public header).

---

## 5. Decisions + recommended direction

### 5.1 Schema as code (the chosen schema fix)
**The C++ type is the single source of truth.** Each validated type gains a
`schema()` method that **builds** its draft-07 JSON Schema programmatically (returns
`nlohmann::json`); the runtime pboettch validator consumes that directly (no embedded
raw-string copy), and a **build/CI step** generates `schemas/*.json` from the type's
`schema()` and fails if the checked-in file is stale. Net effect: define each schema
**once, in the type**; the `.json` files become generated artifacts (kept for docs /
external tooling); the embedded-string copies and their hand-sync burden disappear; and
`run`/`model`/`n8n_workflow` get their now-missing `.json` for free. Migrate the
hand-written `from_json` value types onto the same `schema()` convention so there is one
boundary pattern, and bring `Principal`/`AccessKey` (§2.1) onto it.

### 5.2 RULE-7 tables — add all four
`system.runs`, `system.principals`, `system.keys`,
`system.audit`, computed at query time from the live `Node` accessors, alongside
a panel that renders each (so SQL and the GUI share one source).

### 5.3 Churn consolidation
One `churn_numeric_encoding.hpp` (R1) + route the testkit case and the UC-1 / churn-step
tests through a shared `run_churn_pipeline(config)` core.

### 5.4 Proposed (not yet decided — flagged for a call)
- **Wire RULE-1 into `Node::deploy_graph`** so a programmatic deploy can't bypass the
  gate (today only the GUI enforces it). Likely pair with a PERM-2 capability check.
- **Collapse the 6 provisioners** onto a templated `BurstProvisioner<Config, ControlPlane>`
  base + a shared `json_boundary.hpp` (~375 lines removed), keeping only per-vendor argv
  + config fields. Trade-off: less of the explicit "mirror" readability the pattern
  chose. Decide before touching them.
- **Test-harness consolidation** (R2/R5): migrate the ~10 test files onto
  `testkit::LoopbackCluster` + the shared encoding header.

---

## 6. Where the real risk is (blueprint RSK alignment)

The blueprint's own risk register frames the priority: **RSK-1** (Orama Zetta maturity)
and **RSK-3** (control-plane on a CRDT without transactions/leadership) are the gating
risks, and they map directly to the §1 security/enforcement gaps (leader
election, RULE-1 wiring; CTL-2 mTLS is now built, #466) — those matter more than the
§3 redundancies. The redundancies
(R1–R5) and smells (§4) are **cheap, low-risk paydown** that should happen
opportunistically; the **enforcement wiring** (RULE-1 at `deploy_graph`, PERM-2 checks)
and the **security track** (CTL-1, and CTL-2's enrollment follow-ups) are the
load-bearing follow-ups.

See the recommendations issue for the prioritized, actionable breakdown.
