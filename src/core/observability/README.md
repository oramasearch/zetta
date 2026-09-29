# Observability implementation

The activity feed renders pub/sub topic updates, event updates and event deletions
from the existing audit trail with readable action labels. Payloads are not
included. See [audit trail](../../../docs/governance/audit-trail.md).
Windowed polling totals render as data-access summary activity entries, and an
explicit admin purge renders from its retained `audit_purged` row (#1169).
