# Governance — who may publish, subscribe, and ask for what

**Status:** draft, 2026-09-27, after [D7–D8](decisions.md). Builds on zetta's
existing governance model; adds no second one.

## What zetta already has (`include/zetta/governance/`, `docs/governance/`)

| Piece | What it is |
|---|---|
| `User`, `Role`, `UserRole`, `Token` | the credential store; a request resolves to one principal |
| permission vocabulary (`permission.hpp`) | `<domain>:<action>` atoms (`sql:read`, `blob:read`, `mcp:call`, …) that expand to `Capability{domain, action}` |
| `ScopedGrant` (`grant.hpp`) | an operation bound to one object: `GRANT <perm> ON <object> TO ROLE` |
| `Resource` / `ResourceKind` (`scope.hpp`) | the objects a grant can name: database, project, table, column, blob (by prefix), tool |
| `Policy` (`policy.hpp`) | Postgres-style RLS: a `USING` read filter and a `WITH CHECK` write filter, per role |
| `AuditEvent` | append-only, replicated record of every governed action |
| deny-by-default | nothing is allowed without a grant; admin bypasses |

All of it is **replicated CRDT state**, so it is already cluster-wide: stage 2
inherits it for free.

## How pub/sub plugs in (proposal)

- **A new resource kind, `Topic`**, matched by name or name prefix (like
  `Blob`), so `GRANT pubsub:subscribe ON topic 'orders.*' TO ROLE analyst` works.
- **New permission atoms:** `pubsub:publish`, `pubsub:subscribe`,
  `pubsub:create` (create a topic, including implicitly by auto-creation, D16),
  `pubsub:manage` (drop topics, change their settings). Possibly finer ones
  for delivery options (below).
- **Addresses and wildcards (D26–D27):** every addressable entity has a `kind:name`
  that lines up with `ResourceKind`, so a grant names what it routes to. Sending
  to one address and sending to a **wildcard** (`agent:*`) are separate rights,
  and a wildcard's fan-out is capped and logged. The envelope's `reply-to`
  never proves who sent an event.
- **Acting on behalf (D33, D34):** `from` is the principal the event is for,
  `sender` the stack of agents that sent it, one per delegation. Both are stamped
  by the node; governance checks every link of the stack back to `from`; the
  audit records the whole stack. Maximum depth 10,000 (lower is configurable);
  beyond it the event is dropped with an error logged. *Proposal:* a chain's
  verdict is cached by its hash and invalidated when a covering grant changes.
- **Delivery options are governed like rows.** A `WITH CHECK`-style policy on a
  topic bounds what a publish or a subscription may request (max priority,
  allowed recipient modes, guarantee level), per role. A `USING`-style policy on
  a subscription filters which events a subscriber may receive.
- **Resolution (D7):** the request is layered, most specific first (message →
  subscription → topic default), then the result must pass the governance
  bounds for that principal on that topic.
- **Over-bound requests (D8):** a per-topic setting, `clamp` (default) or
  `reject`. Clamp lowers the request to the bound and delivers; the caller gets
  the effective options back, and the audit records requested vs effective. A
  principal without `pubsub:publish` / `pubsub:subscribe` on the topic is denied,
  never clamped.
- **Every publish/subscribe decision is audited** the way data access is today;
  whether every *event* is audited (volume) is a separate question.

## Stage 1 boundary

Stage 1 is an in-memory data structure. Proposal: it takes an injected
**authorization interface** (`authorize(principal, topic, operation, options)`)
so the structure has no dependency on `zetta::Node`, and the zetta
implementation of that interface calls the existing governance code.
