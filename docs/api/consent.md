# Remote consent for cloud-touching tools

The challenge protocol behind `requires_consent` capabilities (#621; the hard
cloud rule in the root `CLAUDE.md` carried over the wire). Design settled in
[registry-design.md](registry-design.md) → Consent; foundation behavior was
the explicit `consent_required` refusal from
[#644](https://github.com/oramasearch/zetta/issues/644), which this replaces.

## Enforcement is OFF by default

`NodeConfig::enforce_consent` is `false` unless the node was started with
`--governance` (or `ZETTA_GOVERNANCE=1`). With it off, a `requires_consent`
capability executes on the first call and the ungated call is recorded as a
**`consent_bypassed`** audit event carrying the summary the approver would have
read — so "what touched a cloud on this node?" is still answerable without the
challenge.

Off has been the default since [#995](https://github.com/oramasearch/zetta/issues/995),
when no surface a refused caller was sent to could decide a challenge. #999
built those surfaces (below) and briefly proposed turning the gate back on by
default, which would have made every read from a remote source (Snowflake,
Postgres, MySQL…) wait for a second person — including Lake's Connections
pages. **That was decided against**
([#1373](https://github.com/oramasearch/zetta/pull/1373),
[lake#906](https://github.com/oramasearch/lake/issues/906)). What guards a
remote source by default is **safe mode**, set per database: at the default
`warn_writes` a read runs and a write is refused with the exact statement until
the same caller confirms it ([safe-mode.md](safe-mode.md)). Safe mode is a
confirmation by the same person; consent is a second approver, and stays an
opt-in per node.

## Where a challenge is decided (#999)

With the gate on, every surface the `consent_required` refusal names can decide
a challenge:

| Surface | Who decides | How |
|---|---|---|
| **Web console** → Governance → Consent requests (`/?view=governance&tab=consent`) | a signed-in user holding `cloud:manage` (`orchestration:approve_consent`), never the requester | each pending challenge is a card naming what would be read and run, with **Approve and run once** and **Deny**; both POST `/run` as `approve_consent` / `deny_consent`, so the engine authorizes, refuses self-approval, and audits |
| **Desktop app** → Governance (`App::draw_consent_requests`) | the person at the machine | the trusted headed surface (`ConsentSurface::HeadedApp`), exempt from the different-principal rule because it is a human in front of the node |
| **Over the wire** | a key or user of a different principal holding `orchestration:approve_consent` | `approve_consent` / `deny_consent` over MCP or REST |

The requester is never offered Approve on the web page (only **Withdraw**), and
an approval posted by hand from the requester is refused by
`Registry::decide_consent`. With the gate off, both pages say so instead of
showing an empty list.

### A one-person node

- **Headed (the desktop app):** open **Governance**; the pending challenge is at
  the top with Approve / Deny. No second principal is needed.
- **Headless:** create a second user that holds the approver right and sign in
  to the web console as that user to decide:

  ```sql
  CREATE USER approver;
  CREATE ROLE consent_approver;
  GRANT mcp:call, cloud:manage TO ROLE consent_approver;
  GRANT ROLE consent_approver TO approver;
  CREATE TOKEN FOR approver;   -- the secret is shown once; sign in with it
  ```

  `cloud:manage` also grants `cloud_inspect`, `detect_provisioners` and
  `add_nodes`, so give it only to someone trusted to approve those.

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
   the frozen input (with every credential field, such as an inline
   connector's `token`, rendered as `"[redacted]"`; the ledger keeps the real
   value to execute, #74 and [secret-references.md](../governance/secret-references.md)),
   and exactly what would be read/run in the **Cloud
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
   - **The headed app** (`ConsentSurface::HeadedApp`): the Governance page's
     approval surface — the trusted human in front of the machine — set only
     by in-process code, never from request input.
   A token-resolved **user** is an approver like a key: the web console's
   Approve form calls `approve_consent` as the signed-in user. A challenge
   requested by a user records `requested_is_user`, so approval re-authorizes
   and executes as that user through the user path (`may_call_tool` and the
   capability's own right), not the access-key path.
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
| Headed approval surface | `App::draw_consent_requests()` (desktop app, Governance page): each pending card names what would be read/run, Approve / Deny drive `decide_consent` as the trusted surface |
| Web approval surface | `render_consent_requests` in `src/core/api/http_server.cpp` (web console, Governance → Consent requests): pending cards read through `list_consent_requests`, Approve / Deny forms POST `/run` as `approve_consent` / `deny_consent` |
| The enforcement switch | `NodeConfig::enforce_consent` (`include/zetta/node.hpp`, default off), read in `Registry::call`; `--governance` / `ZETTA_GOVERNANCE=1` in `src/main.cpp` |

## Input-aware gating

The describer sees the validated input, so consent is judged per call, not per
tool: `add_nodes {target: "local"}` brings up no machines and touches no cloud
— it executes on the first call, matching the GUI's plain-click parity — while
any cloud target challenges. `create_dataset_from_query` reads its resolved
connector the same way: a Postgres on a loopback host stays on the machine,
so it is not a cloud and is not gated (#994), and a remote
source's challenge names **that connector's** real type and target rather than
a fixed sentence about Snowflake (#995). A `requires_consent` capability without a
describer always challenges with a generic action built from its description
(fail closed).

Consent does not grant connector destination access (#1414). Even an ungated
loopback source requires an exact operator host:port allowance; Unix-socket
sources are rejected. See the
[connector destination policy](../data/connector-query.md#connector-destination-approval-1414-lsr-241).

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
  `system.consent_requests`, or open Governance → Consent requests).
