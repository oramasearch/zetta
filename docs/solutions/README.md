# Solutions

Runtime foundations shared by deployed solutions: experimental measurement,
action policy, monitoring, audit, and durable evidence that connects model scores
and policy decisions to actions and measured outcomes.

- [audit-ledger.md](audit-ledger.md) — the append-only, replicated
  score → decision → action → outcome ledger (#1107), including its API,
  storage, governance, and query contracts.
- [decision-policy.md](decision-policy.md) — versioned score-to-action policy,
  hard constraints, autonomy classes, approvals, batch and request-time decisions
  (#1104).
- [experiments.md](experiments.md) — deterministic holdouts, treatment cells,
  exploration, below-line sampling, exemptions, balance, and uplift rows
  (#1105).
- [monitor.md](monitor.md) — replicated input/score PSI, calibration, closed-window cohort performance, bounded guardrails, breach events, and retrain requests (#1106).
- [roi-readout.md](roi-readout.md) — versioned correctness, aggregate
  incrementality with confidence intervals, customer-supplied ROI, guardrails,
  agent operations, and redacted audit evidence (#1108).
