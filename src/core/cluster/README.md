# Cluster implementation

Peer hello messages advertise the audit protocol. Compatible peers exchange
bounded status/range messages with independent ordinary and summary low-water
marks; incompatible peers retain ordinary catalog
replication but refuse audit. Only the CRDT adapter uses backend codecs to
compact/filter legacy audit history (#1164).
Catalog rebuilds and legacy-audit retirement remeasure the replication memory
counters from the current encoded-value cache; probe subtraction is clamped so
removed catalog history cannot wrap bytes or items (#1167).
Explicit audit purge propagates through the same range exchange (#1169). See
[audit replication](../../../docs/governance/audit-trail.md).
The secret file keeps up to four retired join tokens when `--join-token`
re-keys a node, so the connector-credential key derived from an earlier token
still opens (#1230). See [transport security](../../../docs/cluster/security.md).

`ReplicationManager::update` is an atomic read-check-write under the replication
mutex, and `record:` keys are revision registers: concurrent values resolve to the
highest `rev` in the same pass that makes removal win. `Cluster::request_each`
sends one request to several peers in one round (#1231). See
[revision registers](../../../docs/cluster/replication.md#revision-registers-1231).
