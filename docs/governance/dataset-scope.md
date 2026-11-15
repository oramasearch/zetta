# Dataset scope: confine one call to named datasets

**Symbols:** `zetta::api::DatasetScope`, `DatasetName`, `kDatasetScopeHeader`
(`include/zetta/api/dataset_scope.hpp`, `src/core/api/dataset_scope.cpp`),
`Capability::dataset_reads`, `ApiContext::dataset_scope`, `dataset_scope_refusal`
(`include/zetta/api/registry.hpp`, `src/core/api/registry.cpp`) · **Issues:**
[#1515](https://github.com/oramasearch/zetta/issues/1515), the zetta side of
[lake#1055](https://github.com/oramasearch/lake/issues/1055) (LSR-181).

## Why

Lake confines an agent launch to a **work area**: the datasets the launch was given
(`dataset:<db>/<name>` scopes on the launch, schedule or service row). Its credential
reaches the workspace Zetta through lake, which calls with its own service bearer. Lake
cannot tell which datasets an engine call reads without parsing the input a second time,
so until now it refused every engine call from a confined credential.

The engine parses and runs the call, so the engine decides: the caller names the datasets
the call may read, and the engine refuses anything outside them before it reads a row.

## The header

```
Zetta-Dataset-Scope: sales.accounts, sales/orders
```

Entries are separated by commas and/or whitespace, each `<database>.<table>` or
`<database>/<table>`, both parts made of letters, digits, `_` and `-`. An empty value
confines the call to **no** dataset. A malformed entry or a repeated header is a 400
`invalid_input`; a scope that cannot be read never widens a call.

The scope **only narrows**. The bearer is authenticated and authorized exactly as
without it, and the scope is checked after that, so the header needs no trust of its
own: whoever holds the bearer could make the unconfined call anyway. It is read only for
an authenticated request; without a valid bearer the answer is still 401.

## What a scoped call may do

- **Only registry capability routes answer** (`/v1/<capability>`, GET or POST). `/mcp`,
  the object byte routes (`/v1/objects/...`), `/v1/specs` and the serving routes
  (`/v1/models/...`, `/v1/indexes/...`, `/v1/pipelines/...`) answer 403: none of them
  says what it reads.
- **A capability runs only when it declares what it reads** for its input
  (`Capability::dataset_reads`) and every dataset it reads is in the scope. A capability
  that declares nothing is refused (fail closed).
- **A refusal** is 403 `unauthorized`, audited as a refused call (`api:<capability>`,
  allowed `false`), and its `details` says what the call reads:

```json
{"ok": false, "error": {"code": "unauthorized",
  "message": "'sql' reads sales.refunds, which is outside the dataset scope (sales.accounts)",
  "details": {"dataset_scope": "sales.accounts",
              "datasets_read": ["sales.refunds"], "outside": ["sales.refunds"]}}}
```

`datasets_read` is `null` when the capability cannot say what it reads for that input.

## Capabilities that declare their reads

| Capability | Reads | Refused under a scope |
| --- | --- | --- |
| `sql` | each statement's table, `SELECT` only, with the `database` input as the default | any other statement (writes, DDL, users, tokens, roles, policies), a `system.*` view, the connection and safe-mode stores, a table with no database, an input that does not parse |
| `list_datasets` | only the scope's datasets: the handler leaves every other one out of the answer, `summary` and `full` alike ([#1519](https://github.com/oramasearch/zetta/issues/1519)) | never |
| `get_dataset` | the one dataset named: `database` plus `dataset` taken whole, or `database.table` split at its first dot; a scoped call is resolved exactly that way, with no search across databases (#1519) | a bare name with no `database` (it would be looked up in every database) |

Every other capability is refused under a scope until it declares its reads. These stay refused
because what they read cannot be bounded from the input (#1519):

| Capability | Why |
| --- | --- |
| `score`, `explain` | they also read the model's entity-key table (#1255), which the model names, not the input |
| `analyze_dataset`, `plan_features` | they look for joins across the dataset's whole database |
| `create_dataset_from_query`, `upload_dataset`, `promote_dataset`, `generate_dataset` | they write a dataset, which is outside the scope |
| `run`, `create_study`, `run_solution` and the other pipeline actions | a graph reads whatever its steps name |
| `server_info` | not a dataset read; lake's probe relies on its refusal to tell an engine that enforces the scope |

## Using it from lake

Lake sends the launch's work area as the header on every engine call a confined launch
credential makes (`dataset:<db>/<name>` becomes `<db>/<name>`), and passes a 403's
`details` back to the agent. That is a lake change; until it ships, lake keeps refusing
engine calls from confined credentials.

## Code and tests

- `src/core/api/http_server.cpp` (the auth gate): reads the header after authentication,
  refuses a malformed one (400) and any path that is not a capability route (403), and
  carries the scope to `ApiContext::dataset_scope` for the REST call.
- `src/core/api/registry.cpp` (`Registry::call`): `dataset_scope_refusal` after
  authorization and before anything runs, audited.
- `src/core/api/capabilities.cpp` (`sql_dataset_reads`, `get_dataset_reads`, the scoped `list_datasets` filter): what each declared capability reads.
- `tests/api/test_dataset_scope.cpp`: parsing, the registry decision (inside, outside,
  empty scope, every non-`SELECT` statement, an undeclared capability, the audit row), and
  the scoped `list_datasets` and `get_dataset`, the capabilities that stay refused, and over REST: the header confines, a malformed or repeated header is 400, only capability
  routes answer, and an unauthenticated scoped request is still 401.
