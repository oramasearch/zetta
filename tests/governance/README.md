# Governance tests

Role tests keep the permission vocabulary and built-in mappings synchronized.
They also keep `studies:delete` distinct from Pipeline management and study
promotion (#1158).
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
