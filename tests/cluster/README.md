# Cluster tests

Tests cover catalog convergence, durable removals and solution reconciliation.
Audit records no longer pad catalog fixtures: those use neutral fixture keys,
while audit delivery and legacy-history compaction are tested in governance
(#1164). See [the proof runbook](../../docs/testing/audit-replication.md).
The incremental-reconcile load case migrates 100,000 legacy audit records, then
checks ordinary catalog ticks while new audit records append outside the catalog.
`ReKeyingRetiresTheEarlierToken` covers the bounded retired-token list a
`--join-token` re-key keeps for the connector-credential key (#1230).
`PeerGateway.OverlappingPrivateNodesReplicateThroughOpaqueGateways` models the
two zetta-tunnel gateway hops with byte-forwarding fixtures and proves that
dial-out-only nodes with overlapping private addresses retain end-to-end mutual
TLS and bidirectional CRDT convergence (#1282).
`test_cluster_hello_bounds.cpp` covers the HELLO bounds (#771): the node-id and
`HOST:PORT` checks, the 16-endpoint cap, refusal of HELLOs carrying escape
sequences or too many endpoints over a live plaintext connection (with a
well-formed HELLO as the positive control), the 16 KiB pre-HELLO frame bound, and
the capped advertised set.
`ReplicationDelta.*` (`test_replication.cpp`) prove a gossip delta carries only the
changes a peer lacks, falls back to the whole document, reaches an audit-retired
peer, and converges a four-node mesh with stale clocks and partial views without
leaving a gap (#794). `test_gossip_bandwidth.cpp` checks that steady-state rounds
over mutual TLS ship deltas, and holds the `ZETTA_GOSSIP_BENCH=1` measurement of
bytes per round and convergence time.

`test_cluster_setup.cpp` covers the shared TLS/HELLO deadline, partial-frame
progress, outbound setup, pending-setup refusal and slot recovery, dials that
an inbound flood cannot block, and continued isolated subprocess, observes an actual accept error, releases its temporary
acceptance after descriptor exhaustion (#1401). The descriptor-limit case runs in
files and proves a healthy peer joins. Completed setup remains connected after
its former deadline.
`chaos_harness.hpp` + `test_chaos.cpp` are the chaos experiments (#842): a seeded
schedule of single faults on a 3–5 node mutual-TLS mesh under load, each proven
to have landed and then asserted to heal. They build as their own executable,
`zetta-chaos-tests`, labelled `chaos`; the long runs are opt-in. See
[chaos-experiments.md](../../docs/testing/chaos-experiments.md).
`ClusterSecurity.CertificateCommonNameIsTheNodeId` and
`ClusterSecurity.PeerWhoseHelloIsNotItsCertificateIsRefused` cover the pin of a
peer's `HELLO` node id to the common name of the certificate mutual TLS verified
(#669): a valid member certificate naming another node is refused, while a
matching peer joins the same node in the same window.
`test_cluster_join_authority.cpp` is #848's authority half: two nodes over mutual
TLS with real `Node`s and identity roots, in `main.cpp`'s order (join, sync,
then seed). A joiner with its own `--admin-token` mints no owner the cluster
honors, a joiner with the cluster's token adds no second owner, and a member
that does not hold the root cannot issue identity, through its node or by
writing the replicated catalogue (#763, #764). A member that replays an old
sealed token or role assignment after the owner revoked or removed it changes
nothing, and a member's unsealed removals delete nothing (#1341). See
[identity-seal.md](../../docs/governance/identity-seal.md).
