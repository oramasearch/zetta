# Cluster tests

Tests cover catalog convergence, durable removals and solution reconciliation.
Audit records no longer pad catalog fixtures: those use neutral fixture keys,
while audit delivery and legacy-history compaction are tested in governance
(#1164). See [the proof runbook](../../docs/testing/audit-replication.md).
The incremental-reconcile load case migrates 100,000 legacy audit records, then
checks ordinary catalog ticks while new audit records append outside the catalog.
