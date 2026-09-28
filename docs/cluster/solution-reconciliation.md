# Solution records in incremental reconciliation

Refs #1128 and #1086. The workspace integration combines `deploy/solutions`
with the bus/engine branch and the #1131 signal-handler replacement fix. It
retains the owner wake-up callback and the audit ID index alongside the solution
ledger. No JSON, schema, or wire format changes are introduced.

## Prefix ownership

`Node::tick()` uses `ReplicationManager::changes_since` and
`make_reconcile_batches` for small changes. The solution families participate
according to their existing source of truth:

| Record family | Read/projection path |
|---|---|
| `monitor:` | `Node::reconcile_monitors` updates the handle and monitor file from its changed-key bucket. |
| `experiment:` | `ExperimentManager::reconcile` projects definitions before assignments. |
| `experiment_assignment:` | The same experiment bucket updates assignment records/files without replacing unrelated assignments. |
| `ledger:` | `Node::reconcile_ledger_entries` materializes newly observed action/outcome records. |
| `decision_policy:` / `decision:` | `DecisionPolicyStore` reads the current CRDT snapshot directly; there is no Node projection to reconcile. |
| `roi_readout:` | `RoiReadoutStore` reads versioned definitions directly from the CRDT. |
| Action outbox, dead letters, report publications | Ordinary `db:` / `tbl:` / `row:` records use existing data reconciliation. |
| Snapshot and solution-decision tables | The same data reconciliation handles metadata and rows. |

The direct-read stores deliberately do not acquire a second cache or no-op
reconcile pass. A policy version, approved decision or ROI definition is visible
on the receiving peer from the merged document itself.

## Complete and scoped application

The added reconcile signatures accept `const std::vector<std::string>*
changed_keys`, defaulting to null. Null means apply a complete snapshot, as
before. A non-null list limits additions, updates and removals to those keys.
`reconcile_from_replication` still calls the complete monitor, experiment and
ledger paths for startup, journal gaps and large batches.

A changed monitor cannot remove an untouched monitor just because the untouched
one is absent from its bucket. The same rule applies to experiment definitions
and assignments. Invalid values are refused by the existing schema parsers;
an invalid replacement removes only the affected mutable projection.

Experiment parent changes also include their current assignment keys from the
snapshot. This handles an assignment arriving before its definition: when the
parent arrives, its previously unprojected children become visible in that same
tick. Removing a parent removes its assignment projection and files, without
writing child tombstones back into the CRDT. Re-creating the parent revisits
surviving child records. Unchanged experiments and their children are retained.

The ledger retains its existing immutable, append-only contract, like the
governance audit trail. Neither a full snapshot nor an incremental bucket edits
or removes an already materialized entry. Outcomes remain separate records
referencing their actions. No reconcile pass emits new audit records, publishes
monitor breach events, or captures its materialization back into replication.

## Regression proof

`tests/cluster/test_solution_replication.cpp` seeds 128 unrelated audit records,
starts a receiving Node, merges small changes from a separate replication
manager, and ticks exactly once per assertion. Before each tick it asserts
`full_resync == false` and a key count below the full-reconcile threshold. After
each tick it checks the document version is unchanged by materialization.

`SolutionReplication.*` covers monitor updates/removals and restart; independent
experiment and assignment changes; malformed records; late parents and parent
removal/re-creation; ledger action/outcome arrivals alongside indexed audit
events; direct-read policy versions, approved decisions and ROI definitions;
and action-outbox/snapshot table rows. Existing `IncrementalReconcile.*` tests
retain the large-audit-trail performance proof, and `SignalPlane.*` includes the
in-flight handler-swap regression from PR #1133.

Native static schema, focused and full suites are the integration proof. Shared
configuration is not required (#1096); no container image is built on this host.
