# Releases

One page per tagged release, newest first. A page states what changed, **what it means for
someone upgrading**, and what is knowingly still broken — a release note that lists only
wins is marketing, and the next reader rediscovers the gap expensively.

- [v0.6.0](v0.6.0.md) — not yet tagged. Open-source readiness (MIT license, SPDX headers,
  third-party notices, SBOM, CLA, security policy, client and internal material out, the image
  `Dockerfile` in this repo), the first batch of pre-launch security fixes, and `score`/`explain` on
  segmentation models. **Clerk needs `ZETTA_CLERK_AUTHORIZED_PARTIES`; an inbox needs an
  `inbox:<name>` grant; `webapp/` is gone.** Rolling upgrade is fine.
- [v0.5.0](v0.5.0.md) — 2026-10-23. The security release: the auth review, multi-tenant audit,
  TC-5 and pre-launch review fixes (sealed identity under a cluster root, per-caller catalog and
  search, egress denied by default, approved connector destinations), plus MySQL, TiDB and lake SQL
  connectors, safe mode per database, `WHERE` with `AND`/`OR`/`NOT`, and published pipelines.
  **Upgrade every node together; #1442 cannot be rolled back; roll orama-operator#890 first.**
  Image: `oramasearch/zetta:v0.5.0`.
- [v0.4.0](v0.4.0.md) — 2026-10-19. Datasets: the source's column order and typed columns,
  `LIMIT` pushed to the partitions, text columns as categorical features, the contract typed
  from the partition catalog, and `load_dataset_object` past the 16 MiB inline cap. **Behaviour
  changes:** `get_dataset`'s `columns` are typed objects, and predict takes categorical columns by
  their original name. Image: `oramasearch/zetta:v0.4.0`.
- [v0.3.0](v0.3.0.md) — 2026-10-17. Zetta's part of secure networking: a workspace's
  network records (connections, grants, tunnel sessions, activity), one scoped and
  revocable credential per app connection, Zetta peer links through zetta-tunnel peer
  gateways, and the cluster's mutual-TLS transport as a library zetta-tunnel links. No
  behaviour change for a `v0.2.0` caller. Image: `oramasearch/zetta:v0.3.0` (the same digest
  as `main-d7551c75`).
- [v0.2.0](v0.2.0.md) — 2026-10-12. The engine a hosted workspace runs on: 21 methodologies
  behind one registry, composed solutions, AutoML studies, pub/sub inside the node, a keyed
  record store and an object store, and an audit trail outside the replicated catalogue.
  **Upgrade every node of a cluster together**, and every SQL statement that changes data
  now checks its right (behaviour changes). Image: `oramasearch/zetta:v0.2.0` (the same digest
  as `main-4015ef76`).
- [v0.1.1](v0.1.1.md) — 2026-08-27. A repair release: the Postgres connector path is usable
  for the first time, consent enforcement is **off by default** (behaviour change), entity
  ids no longer wrap to 32 bits, and `list_source_tables` + the `model_from_source` MCP
  prompt make a source browsable and the deterministic path agent-followable.
- [v0.1.0](v0.1.0.md) — 2026-08-12. The first tagged release: intent → Pipeline →
  Verification → Run → ranked scores with reasons, on an equal-peer CRDT store with native
  distributed ML. Note its known defects — **the connector path does not work on this tag**.

## Conventions

### Cutting a release

A release is finished when the Git tag, the GitHub release and the image tag all exist.

1. **On a branch off `dev`**, write `docs/releases/vX.Y.Z.md`, add it to the list above, and move
   `kVersion` in `include/zetta/version.hpp`, `project(... VERSION ...)` in `CMakeLists.txt` and
   `info.version` in `schemas/openapi.json`. `tests/test_release.cpp` (`ReleaseVersion.*`) fails
   until all three agree and this page exists and is listed. Build and run the full suite, and
   put the counts in the page. Open the pull request into `dev`.
2. **Promote `dev` to `main`** with a pull request titled `release: vX.Y.Z — ...`, merged with a
   merge commit, never a squash. `dev` is kept.
3. **Tag the merge commit** `vX.Y.Z` (a lightweight tag, like the earlier releases) and publish
   the GitHub release from the page, with its relative links made absolute at the tag.
4. **Build the image from the tag** (a clean checkout at `vX.Y.Z`, `linux/amd64`) and push it
   as `zetta:vX.Y.Z`. A node built from it logs `zetta X.Y.Z: node started`.

### Writing a release page

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
