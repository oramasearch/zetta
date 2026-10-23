# Governance tests

Role tests keep the permission vocabulary and built-in mappings synchronized.
They also keep `studies:delete` distinct from Pipeline management and study
promotion (#1158).
`BoundedRoles` in `test_roles.cpp` covers bounded role management (#761): a
`roles:manage` holder grants only what it holds (on the whole object, for a
scoped grant), touches no role above it, and nobody changes or drops a built-in.
Pub/sub capability tests additionally exercise the topic/event/subscription scope
matrix. See [governance](../../docs/governance/README.md) and
[CRUD](../../docs/pubsub/crud.md).
Audit tests cover segment migration, index-only membership, independent
ordinary/summary retention, empty polling windows, single-node and replicated
purge, schema round trips, and the million-record RAM/disk budgets (#1154,
#1169).
That million-record test also proves the audit probe moves after 100,000 events
(#1163).
The native `zetta-audit-proof` worker measures RSS independently on two nodes,
each writing 500,000 events and converging to a million, then reopening in fresh
processes. Tests also cover 100,000-key catalog compaction, mixed-version audit
refusal, retained-summary range gaps, replay and malformed frames (#1164).
The catalog-compaction regression compares the replication probe with a fresh
measurement and rejects wrapped byte or item totals (#1167). The worker checks
the memory registry's audit item count after both phases.
See [the proof runbook](../../docs/testing/audit-replication.md).

`test_authz_regressions.cpp` is the authorization regression harness (#780): one
test per auth review finding, #759 to #769. Fixed findings are plain assertions;
open ones are `KNOWN_DEFECT` probes that skip with the issue number while the
defect reproduces and fail once it does not, so the fix flips them. See
[authz-regressions.md](../../docs/testing/authz-regressions.md).

`test_sandbox_escape.cpp` is TC-5 (#252): an analyst with one column grant tries
every listed jailbreak step through `Registry::call`, and each attempt must be
refused and recorded as refused. Open gaps are `KNOWN_DEFECT` probes. See
[TC-5 coverage](../../docs/testing/tc-5-coverage.md).

`test_audit_completeness.cpp` covers completeness and attribution (#847): the
Node's own `data_changed` rows for project, database, model and run changes
made directly (as the headed app does), deploys, jobs and graph changes naming
the declared caller instead of a blank or the project's owner, the caller
carried across `run_on_owner`, a capability's effects attributed to the calling
user, an authorized call that then failed recorded as a failure, and a SQL
statement's effects attributed to its session.
`test_system_catalog_access.cpp` covers who may read each `system.*` table
(#766): admin-only governance and node tables, public metadata, datasets and
partitions narrowed to the caller's tables, each person's own audit rows, and
tables that follow a read tool's scope. See
[system-catalog-access](../../docs/governance/system-catalog-access.md).
