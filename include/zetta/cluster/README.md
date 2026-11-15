# Cluster interfaces

`Cluster` exposes peer audit protocol versions and bounded owner-thread audit
message exchange. `ReplicationManager` restores local actor clocks and retires
legacy audit history without retaining per-event tombstones (#1164).
See [replication](../../../docs/cluster/replication.md).
`ClusterSecurity::retired_join_tokens` lists the tokens a re-keyed node ran
under before (at most four, newest first), so the connector-credential sealing
key derived from an old token still opens and re-seals (#1230). See
[transport security](../../../docs/cluster/security.md).
`EnrollmentCa`, `sign_enrollment_certificate`, `verify_enrolled_certificate`,
`enrollment_proof` and the pin store are CTL-2 V2 enrollment (#1319);
`ClusterSecurity::enroll` turns it on (every zetta process), and
`kNodeCertPrefix` names the replicated enrollment record. Since #1513
`cluster_ca_public_key`, `provision_enrollment_ca`, `enrollment_credentials` and
the raw-key signing helpers carry enrolling without a client certificate, and
`Cluster::enrollment_ca_public_key` is the join command's `--cluster-ca`.
`Cluster::set_revoked_keys` and `NodeRevocation` enforce per-node revocation
(#1320); `node_revocation_record_schema` is its sealed record.
