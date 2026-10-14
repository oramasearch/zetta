# Zetta peer links through peer gateways

Issue [#1282](https://github.com/oramasearch/zetta/issues/1282) connects Zetta
nodes in separate networks through zetta-tunnel peer gateways while preserving
Zetta's node-to-node mutual TLS.

## Deployment contract

The operator projects one Zetta peer listener as a named `PeerConnection`
import. The joining Zetta node uses that private import listener as its ordinary
seed endpoint:

```sh
zetta --peer 127.0.0.1:15477 \
  --dial-out-only \
  --join-token <shared-cluster-token>
```

`--peer` names the reachable path from that node's network. It does not need to
be the remote Zetta listener. The import gateway opens the peer-gateway TLS 1.3
link, selects the authorized named export, and the export gateway opens a TCP
connection to the remote node's cluster listener. After authorization, the
gateways copy application bytes unchanged.

Both private Zetta nodes use `--dial-out-only` when their addresses are not
routable across the networks. Their `HELLO` messages advertise no private
endpoint and their replicated membership entries carry an empty `address` with
`reachability: dial-out-only`. This prevents a learned `10.244.x.x` pod address
from being dialed in another cluster that uses the same address range. The
configured gateway seed is retained as the reached path and is redialed after a
disconnect.

The Zetta join token is delivered to the nodes through their existing secret
contract. It is not sent to either gateway. Zetta performs its own mutual-TLS
1.3 handshake through the forwarded stream, verifies the token-derived cluster
CA at both nodes, and only then exchanges `ZETTA/1` messages. Gateway TLS is an
additional transport layer; it does not terminate or replace Zetta TLS.

## Acceptance proof

`PeerGateway.OverlappingPrivateNodesReplicateThroughOpaqueGateways` starts two
Zetta nodes with the same modeled private endpoint, two local byte-forwarding
gateways, and one shared join token. It proves:

- both nodes suppress the colliding private endpoint;
- the joining node records the private import listener as its reached path;
- the nodes form one mutual-TLS cluster through both gateway hops;
- CRDT writes made on either node converge, including matching document
  versions.

The forwarders do not parse TLS or the Zetta protocol, so successful membership
and replication prove that the Zetta security session remains end to end.

## Boundaries and later work

This change adds no Zetta JSON shape, wire message, credential, or network
record. Lake owns `network_peers` desired state, the operator projects the peer
configuration and policy, and zetta-tunnel owns gateway authorization, gateway
certificates, readiness, and decision records. Sharing Zetta's Corosio transport
implementation with zetta-tunnel remains [#1283](https://github.com/oramasearch/zetta/issues/1283).
