# Audit history without catalog memory growth · `audit-off-catalog`

**Issue:** https://github.com/oramasearch/zetta/issues/1164
**Persona:** operator of a development Zetta cluster retaining audit history.
**Goal:** keep audit records available on peers without holding every event in the catalog.
**Status:** not yet run.

## Before starting

Have an approved development cluster, its previous audit count, and permission
for the exact inspection, all-node upgrade and restart actions. Preserve a
recoverable copy of the existing data. Mixed-version audit delivery is refused.

## Steps

1. Stop old writers and upgrade every node together, then restart the cluster.
   Confirm `system.nodes` reports audit protocol `1` and compatible connected peers.
2. Read older audit records through the existing SQL capability over REST and
   MCP. Confirm the expected history and summary fields remain available.
3. Perform an audited action on each node. Read it from another node and inspect
   `system.node_metrics.audit_watermarks`; sequences should converge by origin.
4. Restart again. Confirm historical records remain readable and new actions
   continue advancing each origin's sequence.
5. Measure the lake-bus Zetta process's total resident memory after restart.
   Confirm it is below 150 MiB. Record the build, audit count, watermarks and RSS.

## Passes if

Historical audit reads survive migration/restart, new audit events converge
between peers, and the live-bus process remains below the total RSS bound.

## Notes for the tester

The automated [native proof](../audit-replication.md) separately requires a
million replicated events on each of two processes, below 100 MiB added
steady-state RSS and 150 MiB added startup peak RSS per process. It does not
substitute for this deployment check. Never downgrade one peer expecting audit
interoperation; the rest of the catalog remains compatible.
