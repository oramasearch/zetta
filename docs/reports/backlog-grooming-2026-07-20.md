# Backlog grooming — 2026-07-20

A full pass over the open issue backlog on [`oramasearch/zetta`](https://github.com/oramasearch/zetta),
oldest first, one issue at a time. Every issue reviewed got a comment citing evidence —
`file:line` references, sibling-issue states, and quotes from headers and docs — whether it
closed or stayed open.

**Status: in progress.** 41 of ~104 issues resolved at the time of writing. This report is
the durable record; the per-issue reasoning lives in the GitHub comments.

## Why the record matters

The pass repeatedly found the same class of problem: **a claim in the product surface with
nothing behind it.** Three examples, each found by checking a claim against the code rather
than against another document:

- `docs/product/positioning.md:9` sells *"zero egress is a testable claim (TC-2)"* in
  competitive material. TC-2 had never been run (#249).
- `src/core/cloud/cloud_provider.cpp:58,73` advertise *"EKS"* and *"GKE"*. No
  cluster-creation code exists in any backend (#114).
- The correctness-gate eval rate is a stated floor before self-service GA. There is no eval
  set (#248).

A fourth was a documentation claim that had gone stale under a shipped feature:
`docs/architecture/code-audit.md:38` asserts *"No inline secrets found… nothing violates the
rule"*, written 2026-07-12; the connector store landed 2026-07-19 writing source tokens in
cleartext to a replicated row (#74, #830).

## Conventions adopted during the pass

Three durable conventions came out of the work and now live in the skills and in
[`docs/testing/test-scripts/README.md`](../testing/test-scripts/README.md):

1. **Every issue carries a manual test script.** A script is something you hand to a human
   tester who follows it and reports pass or fail — a walkthrough of a real user pursuing a
   real use case. It is *not* a unit, integration, or e2e test. Automated green is evidence;
   it is not acceptance. An issue closes as implemented only when a person has run its script
   and it passed.
2. **"Not yet runnable" and "not yet run" are different.** The first means the capability does
   not exist (engineering work); the second means it ships and nobody has walked it (an
   afternoon and a willing person). They should never sit in the same queue.
3. **The word "seam" is banned org-wide** — ambiguous between interface, extension point,
   injected dependency, and boundary. Name the mechanism.

Twelve test scripts were written during the pass and are committed under
[`docs/testing/test-scripts/`](../testing/test-scripts/README.md).

## Verdicts

`KEEP` issues stayed open with a comment recording what ships, what remains, and a test
script. Priority changes are noted.

### Closed

| Issue | Reason | Basis |
|---|---|---|
| #25 `[API-5]` Extension & Capability API | restructured → #843 | Capability API + governed execution shipped; sandbox half rewritten |
| #467 CTL-1/TB-2 extension sandbox | restructured → #843 | same; its 2026-07-02 deferral preserved in #843 |
| #111 `[ENV-2]` Customer Kubernetes | completed | operator `OramaZettaCluster` CRD + chart + `K8sProvisioner` + e2e CRD |
| #189 `[REL-4]` R4 — Tiered storage | duplicate of #138 | container; sole edge is #138 |
| #254 `[TC-7]` SQLite migration fidelity | duplicate of #263 | nothing to verify; no import exists |
| #614 `[mcp]` MCP + REST control surface epic | completed | all seven children + wave #639 closed; checkboxes stale |
| #647 `[web]` Web interface | completed | ~1000 lines, PR #820, ten views, graph editor |
| #657 DistributedFulltext global statistics | duplicate of #851 | re-filed as the user-visible recall defect it is |
| #670 join token through burst provisioners | completed | all six control planes carry it; plaintext caveat obsolete |
| #850 mount web UI at public root | **filed in error** | premise was a stale comment; the ingress already proxies to the node |

### Kept, with a priority change

| Issue | Change | Why |
|---|---|---|
| #23 `[API-3]` Runner interface | p4 → **p2** | live direction: a `LanguageDetector` over SQL / sim-Python / natural language |
| #114 `[ENV-5]` Managed cloud | p4 → **p2** | provisioning a managed cluster + GUI-driven helmfile is unbuilt |
| #138 `[FEAT-7]` Tiered storage | p1 → **p4** | deliberately deferred to its own wave; nothing blocked on it |
| #193 `[REL-8]` the agent programme | p4 → **p1** | the heart of the product, not a north-star deferral |
| #249 `[TC-2]` No egress under capture | p4 → **p2** | Part 1 (what leaves today) is runnable now and substantiates a public claim |
| #252 `[TC-5]` Sandbox escape | p4 → **p2** | the escape class is open and unbounded; known escapes are p1 |
| #554 `[FEAT-11]` Auto-Suggest | p3 → **p2** | it is the curriculum generator the agent trains against |
| #555 `[FEAT-12]` Auto-RL | p3 → **p1** | the main feature; #193 becomes its integration umbrella |
| #556 `[FEAT-13]` Personalized MCP | p3 → **p4** | layers on the other two; least defined |
| #830 credentials plaintext at rest | p2 → **p4** | an accepted temporary shim, sequenced behind #74 |
| #188 `[REL-3]` One-button deploy | p2 → **p3** | (set by the maintainer during the pass) |

### Kept, rescoped

#27 (one binary serving as desktop **and** CLI over the registry) · #74 (builds the secrets
store; #830 converts onto it afterwards) · #75 (the egress control only; #249 owns the proof)
· #112 (reopened — closed in error without a passing script; on-prem cluster bring-up is
unverified) · #113 (built but never verified by a human; #824 blocks its teardown step) ·
#191 (governance keystone — under-defined; first artifact is a scoping document, twelve
dimensions named) · #192 (all panels ship; the cold-user journey has never been walked) ·
#215 (declarative inventory + enrollment + supercluster self-provisioning via the operator and
Karpenter) · #248 (one shared eval corpus, used by both the gate and the agent curriculum) ·
#262 (the coverage map — which claim is verified by which script, and which have nothing) ·
#263 · #265 (the generic-skeleton cliff) · #267 (deferred; #824 blocks its acceptance) ·
#268 (import/compile shipped; modernization becomes training data) · #275 (custom logic must
genuinely compute, under scoped authority) · #276 (the human acceptance story for the
authorization epic) · #277 (governed sharing, now including fine-tuned agents) · #664 (needs a
re-run after six cluster fixes landed) · #669 (four dependants; carries a no-public-fork
warning for the corosio accessor)

### Created

| Issue | Priority | What |
|---|---|---|
| #843 | p3 | Deny-by-default authority over graph/extension execution (replaces API-5 + CTL-1/TB-2) |
| #845 | p3 | Remove the vestigial non-local `DeploymentKind` values |
| #846 | p2 | Intent, description and goals belong to the project definition |
| #847 | p2 | Audit completeness + identity-scoped results |
| #848 | p2 | Unauthorized node cannot join or orchestrate |
| #849 | p1 | Agent interface — talk to your agent, watch it work, stop it |
| #851 | p2 | Paged search over a sharded corpus can silently drop a top-k document |

## Decisions recorded during the pass

- **The agent programme.** #555 is the main feature and where the work lands; #193 keeps the
  programme context and verifies the data and linkage integrate. Shape: #554 curriculum (p2)
  → #555 training (p1) → #849 the interface (p1) → #556 personalization (p4). The reference
  implementation is `homer6/blender-rl`, to be **re-implemented in canonical C++26**
  (distributed, coroutines) rather than embedded — the same "re-implement, don't run"
  principle already governing input languages.
- **Vocabulary: "agent".** An *agent* is something we create; the RLVR agent is the default
  type. An *external agent* (AX) is somebody else's, driving our service — we cannot control
  it, but we anticipate what it needs. Existing AX usage in `ax.md`, `registry.hpp`,
  `llms.hpp` and `app_home.cpp` is correct as-is.
- **The sandbox is real.** It is not a process jail around foreign code — Zetta re-implements
  rather than runs — but a **governance boundary around user-provided scripts**, which must
  not jailbreak. #843 carries it; #252 verifies it adversarially.
- **Tiered storage is a predictive-placement problem** across memory → NVMe → spinning disk →
  S3 → S3 Glacier Instant Retrieval, aimed at the ~6× archive-tier cost gap
  ($0.004 vs $0.023 per GB-month).
- **`DeploymentKind`'s non-local values are vestigial.** Under the operator model you deploy
  the cluster; a graph runs on the cluster. #845 removes them.

## Method

Recorded in the `prune-backlog` and `github-ops` skills. The two rules that changed most
during the pass, both after getting it wrong:

- **The renaming trap.** Absence of the blueprint's noun is not absence of the thing. #23
  asked for a "Runner interface" with Lua/Orly runners — none exist — but the interface had
  shipped as `input::LanguageFrontend`. Search for the capability and its consumers, not the
  name.
- **A container's summary can claim more than its edges.** #189 and #191 are structurally
  identical — one `contains` edge, closed child — and got opposite verdicts. #191's summary
  claimed *"Identity (Okta)"*, which has zero code, with twenty open security issues under its
  heading.
