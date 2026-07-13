# Remote consent for cloud-touching tools

The challenge protocol behind `requires_consent` capabilities (#621; the hard
cloud rule in the root `CLAUDE.md` carried over the wire). Design settled in
[registry-design.md](registry-design.md) → Consent; foundation behavior was
the explicit `consent_required` refusal from
[#644](https://github.com/oramasearch/zetta/issues/644), which this replaces.

## The problem it closes

Cloud-touching actions (a cloud `add_nodes`, a cloud inspect, provisioner
detection) are deny-by-default, ask-every-time in the GUI. The remote surface
needs an equivalent the **calling agent cannot satisfy by itself** — otherwise
an agent auto-approves its own billable actions and the hard rule is hollow.

## The protocol

1. **First call never executes.** A `requires_consent` capability's call runs
   the normal validate → authorize → audit path, then — when its
   `ConsentDescriber` says the input touches a cloud — issues a **one-time
   consent challenge** on the node's ledger and fails with code
   `consent_required`. The challenge rides as structured `error.details`:
   the frozen input, and exactly what would be read/run in the **Cloud
   Inspector grammar** (`action.summary` · `action.reads` = the config paths
   the provider CLI may read · `action.runs` = the commands / billable
   create). The calling key repeating the call only ever mints another
   challenge.
2. **Approval must be distinctly privileged.** `Registry::decide_consent`
   accepts a decision from two surfaces only:
   - **An approver key** (`approve_consent {challenge_id}` over MCP): a
     NAMED key holding `orchestration:approve_consent` that belongs to a
     **different principal** than the requester. The same key and a sibling key
     of the same principal are both refused — the calling key can never approve
     its own cloud action.
   - **The headed app** (`ConsentSurface::HeadedApp`): the Admin panel's
     approval surface — the trusted human in front of the machine — set only
     by in-process code, never from request input.
   Denial (`deny_consent`) is fail-safe and looser: the requester may
   withdraw its own challenge.
3. **Approval executes exactly the named action once.** The challenge is
   consumed atomically (Pending → Approved, refused for any other state, so a
   replayed approval can never run it twice), the requester is re-authorized
   (its key may have been dropped since), and the **frozen** input executes
   under the requester's identity. Slow cloud halves (CLI shell-outs,
   provisions) run on a background worker; the outcome lands on the challenge
   (`executed` / `failed` + result JSON), applied to the Node on the owner
   thread at the embedding's `BackgroundActions` prune.
4. **Per-action, never remembered, audited on both sides, visible as spans.**
   The ledger is ephemeral (never persisted, never replicated; challenges TTL
   out — default 10 minutes). The challenge audits as `consent_requested`
   under the requester; the decision as `consent_decided` under the approver;
   the execution as a `data_access` under the requester. The challenge is an
   attribute on the call's span and the decision is its own
   `api:consent_decision` span, so both are trace events.

## The pieces

| Piece | Where |
|---|---|
| `ConsentAction` / `ConsentChallenge` / `ConsentLedger` | `include/zetta/governance/consent.hpp` — the governance half; `Node::consent_ledger()` owns one (ephemeral, owner-thread-only) |
| Challenge schema | `consent_challenge_schema()` (schema-as-code) → generated `schemas/consent_challenge.schema.json`, drift-checked |
| Protocol | `Registry::call` (the consent branch) + `Registry::decide_consent` in `src/core/api/registry.cpp`; `ApiError.details` carries the challenge |
| Cloud tools + consent tools | `src/core/api/cloud_capabilities.cpp` — `cloud_inspect` · `detect_provisioners` · `add_nodes` · `list_consent_requests` · `approve_consent` · `deny_consent` (table: [README.md](README.md)) |
| Provisioner backends | `zetta::cloud::ProvisionerFleet` (`include/zetta/cloud/provisioner_fleet.hpp`, moved from the GUI) — one holder feeds the headed Add-nodes control AND `ApiServer::set_provisioner_resolver` |
| Read views | `system.consent_requests` (RULE-7, computed from the ledger) + the `list_consent_requests` capability |
| Headed approval surface | `App::draw_consent_requests()` (Admin panel): each pending card names what would be read/run, Approve / Deny drive `decide_consent` as the trusted surface |

## Input-aware gating

The describer sees the validated input, so consent is judged per call, not per
tool: `add_nodes {target: "local"}` brings up no machines and touches no cloud
— it executes on the first call, matching the GUI's plain-click parity — while
any cloud target challenges. A `requires_consent` capability without a
describer always challenges with a generic action built from its description
(fail closed).

## Boundaries

One new JSON format: the consent challenge, produced locally and returned to
callers — `consent_challenge_schema()` is its schema-as-code source of truth
and `to_json` is validated against it in tests. Everything the tools ingest
rides the registered draft-07 input schemas (the pboettch boundary in
`Registry::call`); the cloud results reuse the existing validated shapes
(`CloudInspectReport`, `ProvisionerConfig`, `ProvisionedNode`).

## Deliberately later slices

- An L2 protocol journey driving challenge → approval over real HTTP with two
  bearer tokens (the L1 in-process journey is `tests/api/test_consent.cpp`).
- Pairing the approval with the burst cost estimate (`BurstApprovalGate` /
  APR-3) so a challenge carries the cost preview alongside the command list.
- Notification of pending challenges (today: poll `list_consent_requests` /
  `system.consent_requests`, or see the Admin panel).
