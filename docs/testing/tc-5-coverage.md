# TC-5 coverage: what denies each jailbreak step, and the test that proves it (#252)

[TC-5](https://github.com/oramasearch/zetta/issues/252) says a low-privilege caller cannot
get out of its capability scope, and that every attempt is refused and recorded. The
human pass is [`tc-5-jailbreak-attempt`](test-scripts/tc-5-jailbreak-attempt.md). This
page maps each of its steps to the code that refuses it and the automated test that
proves the refusal. Gaps that are still open are listed with their issue.

The in-process TC-5 suite is
[`tests/governance/test_sandbox_escape.cpp`](../../tests/governance/test_sandbox_escape.cpp)
(`SandboxEscape.*`). It runs every attempt as an analyst who holds `mcp:call` plus
`sql:read ON app.open(id, region)`, through `Registry::call`, the one entry point REST
and MCP share. An open gap is a `KNOWN_DEFECT` probe: it skips while the gap
reproduces, and fails once it stops so the probe is replaced with a plain assertion.

State at `dev` `e9587213` (2026-10-21), with the open pull requests named where they
carry the refusal.

| Step | Attempt | What refuses it | Test |
|---|---|---|---|
| 1-2 | Malformed, absurd or huge statements take the node down | Any exception a handler throws is contained in `invoke_contained` (`src/core/api/registry.cpp`). The parser stops throwing at all in PR [#1336](https://github.com/oramasearch/zetta/pull/1336) (#793, #819 closed) | PR #1336: `ParseErrorsNeverCrash.*`, `ParseErrorsNeverCrashOnANode.*`. Here: `IndirectSqlFormsAreRefusedAndTheNodeStaysUp` |
| 3 | Read a table with no grant | `Executor::execute`, SELECT: `column_scope` before the table lookup | `ScopedGrantSql.ReaderCannotReadAnUngrantedTable`; `SandboxEscape.ReadingAnUngrantedTableIsRefusedAndTheRefusalIsAudited` |
| 3 | Learn whether a hidden table exists from the error | **Fixed here (#252)**: SELECT, INSERT, UPDATE, DELETE and TRUNCATE check the right before the lookup, so a missing table, a missing database and a hidden table get the same refusal | `SandboxEscape.TheRefusalDoesNotSayWhetherAHiddenTableExists`, `ACallerWithARightStillLearnsATableIsMissing` |
| 4 | Join, subquery, view, CTE, UNION | Not in the SQL subset: refused as a statement | `SandboxEscape.IndirectSqlFormsAreRefusedAndTheNodeStaysUp` |
| 4-5 | Recover a masked column through WHERE, ORDER BY, GROUP BY, MIN/MAX/SUM/AVG/COUNT | The column-grant guard in the SELECT branch (#760 closed, #1021) | `ScopedGrantSql.FilteringOnAMaskedColumnIsDenied`; PR [#1325](https://github.com/oramasearch/zetta/pull/1325) `AuthzNode.Finding760_*`; `SandboxEscape.AMaskedColumnCannotBeRecoveredIndirectly` |
| 6 | A row policy with no condition admits everything | **Open: [#765](https://github.com/oramasearch/zetta/issues/765)** | PR #1325 `Finding765_*` (`KNOWN_DEFECT`); `tests/governance/test_row_security.cpp` |
| 7 | Read the catalogue: users, roles, policies, audit, datasets | **Fixed: [#766](https://github.com/oramasearch/zetta/issues/766)** (PR #1358): each `system.*` table has a reader class, so a non-admin reads only its own audit rows and only the tables it may read ([system-catalog-access.md](../governance/system-catalog-access.md)) | PR #1325 `Finding766_*`; `SandboxEscape.TheAnalystCannotReadTheWholeAuditTrailOrCatalog` |
| 8 | Mint a token, grant a role, edit a role, create a user or policy | Per-statement rights (#759 closed by #1229); role statements need `roles:manage` | `SqlWritePermissions.*`; PR #1325 `Finding759_*`; `SandboxEscape.TheAnalystCannotGiveItselfMoreAccess`. **Open: [#761](https://github.com/oramasearch/zetta/issues/761)**, a `roles:manage` holder can grant itself admin (PR #1325 `Finding761_*`, `KNOWN_DEFECT`) |
| 9 | Reach the network | Capabilities that dial out need their own orchestration right (registry gate). Outbound egress is deny-by-default in PR [#1321](https://github.com/oramasearch/zetta/pull/1321). Each graph step runs as the run's principal in PR [#1330](https://github.com/oramasearch/zetta/pull/1330) (#843). `agent_run` runs only an approved plan in PR [#1337](https://github.com/oramasearch/zetta/pull/1337). **Open: [#1323](https://github.com/oramasearch/zetta/issues/1323)**, egress is node-wide, not per principal | `SandboxEscape.CapabilitiesThatReachOutsideTheNodeAreRefusedAndAudited`; PR #1321 `EgressPolicy.*`, `NativeHttpTransportFixture.RefusesAnOutsideAddressByDefault`; PR #1330 `StepAuthorityNode.*`; PR #1337 `AgentRunRefusesAPlanNobodyApproved` |
| 10 | Read or write a file, run a command | SQL has no file reader or `COPY`; `add_dataset` needs `orchestration:add_dataset`. **Open: [#1323](https://github.com/oramasearch/zetta/issues/1323)**, a holder of that right reads any path the node can | `SandboxEscape.IndirectSqlFormsAreRefusedAndTheNodeStaysUp`, `CapabilitiesThatReachOutsideTheNodeAreRefusedAndAudited` |
| 11 | Use a saved connection never granted | The connection capabilities need their orchestration right; SQL never reaches `zetta_connect` (#1230); `system.connections` prints no credential. **Open: [#1339](https://github.com/oramasearch/zetta/issues/1339)**, no per-connection grant: any holder of the right uses any connection by name | `SandboxEscape.CapabilitiesThatReachOutsideTheNodeAreRefusedAndAudited`, `TheConnectionStoreIsNotReachableFromSql`; `SqlWritePermissions.ConnectionStoreIsNotReachableThroughSql` |
| 12-13 | Is the refusal recorded? | A refused gate call records `data_access` `allowed: false` (registry). A refused write records `data_changed` refused (#1229). **Fixed here (#252)**: a refused SELECT records `data_access` `allowed: false`, `sql:table:<db>.<table>`; before, only the gate's "sql was called" row existed. Effects attributed to the caller in PR [#1333](https://github.com/oramasearch/zetta/pull/1333) (#847) | Every `SandboxEscape` test counts its refused rows; `SqlWritePermissions.DataChangesAreAuditedAsActions`; PR #1325 `Finding769_RefusedPrivilegedSqlIsAudited` |
| 14 | Alter or delete the record | The `system` database is read-only to every caller | `SandboxEscape.TheAuditTrailCannotBeAlteredFromSql` |
| 15 | Off-script | Human only | The findings of this mapping are #1339 and the two fixes above |

What stays human: step 15, and timing as a channel (no automated test measures it).
