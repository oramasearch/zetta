# The Workspace Profile (#886)

The **Workspace Profile** is the input to the *Discover Opportunities* pathway —
the home's second door beside *Create a Solution*. A user describes their
workspace **role** and their plain-language **daily work**, and approves the data
**sources** the environment scan may read; the Auto-Suggest model
([#554](https://github.com/oramasearch/zetta/issues/554)) later consumes that
profile to rank Opportunities the environment can actually support. The profile
is real replicated state immediately; until Auto-Suggest ships, the Opportunities
surface stays honestly in-training.

Boundary: **no new JSON format beyond the entity's own schema.** The profile is a
boundary-validated replicated entity modeled exactly like
[`zetta::ml::Model`](../ml/models.md).

## The entity

`zetta::workspace::WorkspaceProfile`
(`include/zetta/workspace/workspace_profile.hpp`,
`src/core/workspace/workspace_profile.cpp`) is a **top-level replicated entity**
with no parent. Its value:

| Field | Meaning |
|---|---|
| `owner` | the owning principal id (required, non-empty; server-set to the caller) |
| `role` | the user's workspace role (e.g. "Customer success lead") |
| `daily_work` | plain-language description of their daily work |
| `sources` | approved source refs — a JSON array, each a free-form `{ id, name, kind }` (dataset/connection) |
| `created` / `updated` | ISO-8601 UTC stamps (created is fixed; updated moves on each edit) |

Like the rest of zetta's state it is **replicated**:

- **Replicated state.** One flat key per profile,
  `profile:<uuid> → { owner, role, daily_work, sources, created, updated }`
  (`src/core/catalog.hpp`). On each `tick()` the node reconciles
  (`reconcile_workspace_profiles`), so a peer's profile (or an edit/drop)
  materializes locally and versions converge conflict-free.
- **On-disk projection.** `<profile-uuid>/workspace_profile.json` holds
  `{ id, owner, role, daily_work, sources, created, updated }`.
- **Boundary validation.** Both load paths — a `workspace_profile.json` file and
  a replicated `profile:` value — parse through the draft-07 schema
  (`workspace_profile_value_schema()`, registered in
  `zetta::schemas::boundary_schemas()` → `schemas/workspace_profile.schema.json`)
  and **skip** anything that fails, so a malformed payload is dropped rather than
  allowed to corrupt the projection.

`Node` owns the collection (`create_workspace_profile` / `find_workspace_profile`
/ `drop_workspace_profile` / `workspace_profiles()`); create stamps `created` and
requires a non-empty `owner`. Edits go through the handle's `set_content` (role,
daily-work, sources + a fresh `updated`), which captures into the CRDT and
rewrites the file.

## The read view

`system.workspace_profiles` (RULE-7) is computed at query time from
`Node::workspace_profiles()` — one row per profile with `workspace_profile_id`,
`owner`, `role`, `daily_work`, `sources` (the array), `source_count`, `created`,
`updated`. Plain name, no `zetta_` prefix.

## The capabilities (governed)

| Capability | Kind | Operation | What it does |
|---|---|---|---|
| `get_workspace_profile` | Read | `api:query` | one profile's full validated record by uuid |
| `create_workspace_profile` | Action | `orchestration:create_workspace_profile` | create the **caller's** profile (owner is server-set); replicates |
| `update_workspace_profile` | Action | `orchestration:update_workspace_profile` | update role / daily-work / sources by uuid; owner + created stay fixed |

Both actions are in the `pipelines:manage` permission atom
(`src/core/governance/permission.cpp`), so every registry Orchestration op stays
grantable (the admin-covers-every-op invariant, guarded by
`tests/api/test_auth_gate.cpp`). Reads need only `api:query` (`mcp:call`).

## Studio

The Studio *Discover Opportunities* screens (design repo `src/studio/discovery/`)
read the profile through `get_workspace_profile` and edit it through
`create`/`update`; the Opportunities list is rendered honestly in-training until
Auto-Suggest lands. Cross-reference: the design's `discoveryContracts.js`
(`workspace-profile`, `workspace-scan`, `opportunities`).
