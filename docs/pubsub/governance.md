# Governance: who may publish, subscribe and ask for what

**Issue:** [#1029](https://github.com/oramasearch/zetta/issues/1029) · **Decisions:** D7,
D8, D27, D33, D34 · **Code:** `include/zetta/pubsub/authorizer.hpp`, `delivery.hpp`
(`apply_bounds`), `src/core/pubsub/pubsub.cpp` · **Tests:** `tests/pubsub/test_delivery.cpp`,
`tests/pubsub/test_pubsub.cpp`

Pub/sub decides nothing about rights itself; it asks an injected **`Authorizer`**:

CRUD (#1136) separates `topics:read`, `topics:write`, and `events:write`;
`pubsub:subscribe` remains independent. Existing write grants remain compatible.
Per-topic `bounds` further restrict the authorizer's delivery bounds. Mutation
audit records are wired into the node's owner-thread audit sink and contain
hashes, never payloads. See [crud.md](crud.md) for exact scope and audit contracts.

- `authorize(principal, operation, target)` for `pubsub:publish`, `pubsub:subscribe`,
  `pubsub:create`, `pubsub:manage` and the separate `pubsub:send_wildcard`;
- `bounds(principal, target)`: the `DeliveryBounds` (max priority, allowed recipient
  modes, max recipients, max guarantee) that principal may ask for on that target;
- `may_act_for(actor, principal)`: checked for **every link** of the sender stack.

`AllowAllAuthorizer` is for engine-internal use and tests; the node supplies one backed by
the central governance model.

**Identity, not the request, fills the envelope** (D33): `publish(identity, request)`
stamps `from` with `identity.principal` and `sender` with `identity.acting_chain`. A
chain deeper than 10,000 (or the configured `max_sender_chain`) is dropped with an
`ERROR` log line (D34).

**Over-bound requests** (D8): per topic, `Clamp` (default) lowers each field to its bound
and reports it in `PublishResult::clamped`; `Reject` refuses the target with the rule's
text. A principal with no right at all is refused, never clamped. Refusals, clamps,
auto-creation and wildcard sends go to the audit injection point
(`set_audit_sink`).
