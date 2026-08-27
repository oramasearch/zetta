# Releases

One page per tagged release, newest first. A page states what changed, **what it means for
someone upgrading**, and what is knowingly still broken — a release note that lists only
wins is marketing, and the next reader rediscovers the gap expensively.

- [v0.1.1](v0.1.1.md) — 2026-08-27. A repair release: the Postgres connector path is usable
  for the first time, consent enforcement is **off by default** (behaviour change), entity
  ids no longer wrap to 32 bits, and `list_source_tables` + the `model_from_source` MCP
  prompt make a source browsable and the deterministic path agent-followable.
- [v0.1.0](v0.1.0.md) — 2026-08-12. The first tagged release: intent → Pipeline →
  Verification → Run → ranked scores with reasons, on an equal-peer CRDT store with native
  distributed ML. Note its known defects — **the connector path does not work on this tag**.

## Conventions

- **Every number is measured**, against a running node, and says what it was measured on. A
  figure with no run behind it does not go in.
- **A behaviour change gets its own section at the top**, with the flag or setting that
  restores the old behaviour.
- **Known issues are part of the release**, linked to their tracking issue.
- Cross-repo references are full URLs; issues in this repo are `#NNN`.
