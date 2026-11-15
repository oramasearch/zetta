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
Enrollment (#1319): `security.cpp` holds the enrollment CA (random, never from
the token), signs and verifies enrollment certificates and keeps the pins;
`cluster.cpp` pins every enrolled node id to its key at the handshake and runs
the enrollment upkeep loop. Since #1513 an enrolling cluster's transport trusts
only the enrollment CA (`rebuild_transport_locked`): the listener lets a client
without a certificate reach `answer_enrollment` and nothing else, and a node that
is not enrolled dials only to `enroll_through` a member it checked against
`--cluster-ca`. A token change keeps the node's key, so its id stays pinned to it.
Revocation (#1320): `Cluster::set_revoked_keys` takes the revoked keys the Node
trusts; the handshake refuses them, the link writer closes a live link
presenting one, and `ENROLL` refuses them.

`ReplicationManager::update` is an atomic read-check-write under the replication
mutex. `Cluster::request_each` sends one request to several peers in one round
(#1231). Continuously written state stays out of the document; see
[replication](../../../docs/cluster/replication.md#what-does-not-belong-in-the-document-1231).
