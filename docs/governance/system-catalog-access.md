# Who may read the system catalog (#766)

The `system` database's tables, and the spec index, are answered per caller.
Before [#766](https://github.com/oramasearch/zetta/issues/766) every
`system.*` table was dispatched before any identity check, so a caller holding
only `mcp:call` read every user, role, grant, token, the text of every row
policy (with the literal values in its conditions), the whole audit trail and
every dataset name, including tables it had no right on. `/v1/specs`,
`list_specs` and `get_spec` listed every tool while `tools/list` listed only the
caller's.

## The rule

Two identities read every system table whole: the **trusted local session**
(an empty acting id: the library, the headed app, tests) and an **admin**, a
caller that holds `cluster:admin` (`api:cluster_admin`; the built-in `admin` role
and the bootstrap owner do). For everyone else each table has one reader class,
declared on its entry in `kSystemTableCatalog`
(`src/core/query/executor.cpp`, `SystemTableAccess`). `Admin` is the first
enumerator and the default, so an entry added without a class fails closed.

| Class | Tables | A non-admin caller gets |
|---|---|---|
| Public | `tables`, `test_cases`, `cloud_providers` | every row: static product metadata, no workspace data |
| Tool | `models` (`list_models`), `runs` (`list_runs`), `jobs` (`list_jobs`), `indexes` (`list_indexes`), `ledger` (`list_ledger`), `consent_requests` (`list_consent_requests`), `connections` (`list_connections`), `pubsub_topics` (`list_topics`), `deployments` (`deploy`, which writes them) | every row, if it may call the named capability: `mcp:call` on that tool and the capability's own right when it needs more (`orchestration:list_connections`, `topics:read`, `orchestration:deploy`). SQL is not a way around a tool's scope |
| Datasets | `datasets`, `dataset_partitions` | only the datasets it holds `sql:read` on, at table scope or on any column (the rule a SELECT on the table uses), and only those datasets' partitions |
| OwnRows | `audit`, `activity` | only the rows it is the actor of. Two people asking the trail the same question each see their own actions ([#847](https://github.com/oramasearch/zetta/issues/847) step 6) |
| Admin | `users`, `roles`, `user_roles`, `policies`, `tokens`, `keys`, `principals`, `nodes`, `node_metrics`, `memory`, `usage`, `plans`, `workspace_profiles`, `large_objects`, `checkpoints`, `cloud_status`, `provisioners`, `pipeline_jobs`, `collective_rounds`, `spans`, `traces`, `crashes` | `permission denied: system.<t> is readable by an admin only (cluster:admin)` |

A refusal is decided before the rows are computed, so a `WHERE`, an aggregate or
a legacy `zetta_<table>` spelling gets the same answer. An unknown system table
is still the catalog error that lists the tables. The rights are read through
`Executor::require_permission`: a token-resolved user against its roles, a
legacy AccessKey against its own capabilities.

## The spec index

`spec_index`, `find_spec` and `openapi_document` take a `CapabilityVisible`
predicate (`include/zetta/api/rest.hpp`, `include/zetta/api/specs.hpp`).
`GET /v1/specs`, `GET /v1/specs/<name>`, `list_specs` and `get_spec` pass
`callable_by(node, acting_key, acting_is_user)`: for a token-resolved user, the
per-tool `mcp:call` scope (`Node::may_call_tool`), the rule `tools/list` and
`Registry::call` apply. A tool the caller may not call is missing from the
index, its `<tool>.input` / `<tool>.output` are `not_found` like a name that
does not exist, and the `openapi` entry holds only the visible paths. Boundary
schemas are entity shapes, not tools, and stay listed.

`GET /v1/openapi.json`, `/llms.txt`, `/llms-full.txt` and `/llms/<topic>` are
registered before the auth gate on purpose (#618, #645: an agent reads them to
learn how to authenticate), so they cannot be filtered by caller and still
describe every capability of the build. They hold the build's API shape, the
same on every node, and no workspace data. Whether they should move behind the
gate is a separate decision, filed as a follow-up on #766.

## Lake

Lake reaches a workspace Zetta only with the workspace's own admin credential
(`WorkspaceService::call` in lake's `src/control/zetta_workspace.cpp`), and its
agents and apps go through `workspace_query` / `workspace_sql` on that path, so
this changes nothing Lake reads (checked 2026-10-21 against lake `dev`).

## Tests

- `tests/governance/test_system_catalog_access.cpp`: the governance registry
  and the node internals are admin-only, public metadata is open, datasets and
  partitions are narrowed to the caller's tables, each person reads only their
  own audit and activity rows, a Tool table follows its tool's scope, an unknown
  user reads only public metadata.
- `tests/api/test_scoped_grants_api.cpp` (`SpecDiscoveryShowsOnlyCallableTools`)
  and `tests/api/test_rest_server.cpp`
  (`SpecIndexShowsOnlyTheToolsTheCallerMayCall`): the spec index per caller,
  through the registry and over HTTP.
