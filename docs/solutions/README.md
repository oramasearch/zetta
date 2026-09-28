# Solutions

Runtime foundations shared by deployed solutions: durable evidence that connects
model scores and policy decisions to actions and measured outcomes.

- [audit-ledger.md](audit-ledger.md) — the append-only, replicated
  score → decision → action → outcome ledger (#1107), including its API,
  storage, governance, and query contracts.
- [decision-policy.md](decision-policy.md) — versioned score-to-action policy,
  hard constraints, autonomy classes, approvals, batch and request-time decisions
  (#1104).
- [action-integration.md](action-integration.md) — report publication, replicated
  action outbox, webhook and CRM connector plugins, bounded retries,
  acknowledgements, dead letters, holdout suppression, and kill-switch guarded
  closed-loop execution (#1109).
