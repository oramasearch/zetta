# Solution operations

Runtime foundations shared by every deployed solution: monitoring, policy,
holdouts, registry metadata, and audit integration. Each foundation stays
independent so parallel work can call its public C++ interface without linking
private implementation details.

- [monitor.md](monitor.md) — replicated input/score PSI, calibration, closed-window cohort performance, bounded guardrails, breach events, and retrain requests (#1106).
