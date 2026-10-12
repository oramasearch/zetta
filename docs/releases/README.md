# Releases

One page per tagged release, newest first. A page states what changed, **what it means for
someone upgrading**, and what is knowingly still broken — a release note that lists only
wins is marketing, and the next reader rediscovers the gap expensively.

- [v0.2.0](v0.2.0.md) — 2026-10-12. The engine a hosted workspace runs on: 21 methodologies
  behind one registry, composed solutions, AutoML studies, pub/sub inside the node, a keyed
  record store and an object store, and an audit trail outside the replicated catalogue.
  **Upgrade every node of a cluster together**, and every SQL statement that changes data
  now checks its right (behaviour changes).
- [v0.1.1](v0.1.1.md) — 2026-08-27. A repair release: the Postgres connector path is usable
  for the first time, consent enforcement is **off by default** (behaviour change), entity
  ids no longer wrap to 32 bits, and `list_source_tables` + the `model_from_source` MCP
  prompt make a source browsable and the deterministic path agent-followable.
- [v0.1.0](v0.1.0.md) — 2026-08-12. The first tagged release: intent → Pipeline →
  Verification → Run → ranked scores with reasons, on an equal-peer CRDT store with native
  distributed ML. Note its known defects — **the connector path does not work on this tag**.

## Conventions

- **Before working an issue, look for an open PR against it** — `gh pr list --search "<n>"`.
  A contributor's branch that already fixes it is worth reviewing and merging, not
  duplicating. This was missed for [#992](https://github.com/oramasearch/zetta/issues/992)
  in v0.1.1, and an outside contributor's eight-day-old PR was superseded by work written
  from scratch.
- **Credit work the history will not show.** When a fix lands under a different commit than
  the one that first proposed it, the release note says whose it was.

- **Every number is measured**, against a running node, and says what it was measured on. A
  figure with no run behind it does not go in.
- **A behaviour change gets its own section at the top**, with the flag or setting that
  restores the old behaviour.
- **Known issues are part of the release**, linked to their tracking issue.
- Cross-repo references are full URLs; issues in this repo are `#NNN`.
