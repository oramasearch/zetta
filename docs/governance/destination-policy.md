# Destination policy: where an author-chosen URL may go

[#1394](https://github.com/oramasearch/zetta/issues/1394) (LSR-161 of the Lake
pre-launch security review).

## The problem

An action connector's `endpoint` (a webhook, a Salesforce or HubSpot task API)
and a solution's report delivery endpoint are URLs a workspace writer stores.
The node dials them later, from inside its own network, with the delivery's
bearer token in the `Authorization` header. Before this change the native HTTP
client connected to whatever the host resolved to, so a stored endpoint could
name:

- the node's own loopback services (`127.0.0.1`, `localhost`, `::1`);
- a pod or service on the cluster network (`10.x`, `172.16.x`, `fd00::/8`);
- a cloud metadata endpoint (`169.254.169.254`, `fd00:ec2::254`).

and the delivery outcome in the ledger told the author what answered. A network
policy around the pod may block those, or may not: a Lake's Zettas run with
`ZETTA_EGRESS_ALLOW=*` and rely on the namespace's NetworkSandbox.

## The rule

A request whose URL a caller chose is marked
`HttpRequest::destinations = DestinationPolicy::PublicOnly`. Every action
connector request is (`add_common_headers` in
`src/core/solutions/action_integration.cpp`), which covers `deliver_task`,
`execute_closed_loop`, `publish_report` and the deferred report delivery of a
solution run.

For such a request the native client (`perform_url` in
`src/core/api/native_http_transport.cpp`):

1. resolves the host once;
2. classifies every resolved address with `connect::classify_address`;
3. skips each address whose class is not allowed;
4. connects only to an address that passed, so the address it checked is the
   address it dials: no second lookup, so a DNS answer that changes between the
   check and the connect (DNS rebinding) cannot redirect it;
5. refuses with `destination refused: <host> resolves to <address>, a <class>
   address ...` when no resolved address is allowed, before any connection;
6. applies the same steps to every redirect hop, because each hop re-enters
   `perform_url`.

| Class | Addresses | Allowed by default |
|---|---|---|
| `public` | every globally routable unicast address | yes |
| `loopback` | `127.0.0.0/8`, `::1` | no |
| `private` | `10/8`, `172.16/12`, `192.168/16`, `100.64/10`, `fc00::/7`, `fec0::/10` | no |
| `link-local` | `169.254/16`, `fe80::/10`, except the metadata addresses | no |
| `metadata` | `169.254.169.254`, `169.254.170.2`, `100.100.100.200`, `fd00:ec2::254` | no |
| `reserved` | `0/8`, multicast, broadcast, documentation, benchmarking, `240/4`, `2002::/16`, `2001::/32` and the like | never |

An IPv6 address that carries an IPv4 one (`::ffff:a.b.c.d`, `::a.b.c.d`,
`64:ff9b::/96`) is classified by that IPv4 address, so `::ffff:127.0.0.1` is
loopback. Allowing `link-local` does not allow `metadata`.

Model-weights fetches and Clerk JWKS keep `DestinationPolicy::Any`. Warehouse
connectors use the independent `DestinationPolicy::Connector`: private and
loopback addresses require an exact operator-approved host:port; metadata and
link-local addresses are always denied. See the
[connector policy](../data/connector-query.md#connector-destination-approval-1414-lsr-241).

## Configuration

```
zetta --action-allow-destinations loopback,private
ZETTA_ACTION_ALLOW_DESTINATIONS="loopback private"
```

A comma- or space-separated list of `loopback`, `private`, `link-local`,
`metadata`. Empty or unset allows nothing beyond public addresses. The flag wins
over the environment; an unknown class stops the node at startup. The node
prints `zetta: action delivery destinations: public only` (or the list) when it
starts.

Allow a class only when a delivery target really lives there, for example an
on-premises install whose CRM gateway is on a private network.

## Relation to the egress allowlist

The node-wide egress allowlist
([#1321](https://github.com/oramasearch/zetta/pull/1321), `ZETTA_EGRESS_ALLOW`)
decides which destinations the node may reach at all. This policy is narrower and
per request: even where egress is open (`*`), an author-chosen delivery URL
reaches only public addresses. Both checks apply when both are present.

## Tests

- `tests/connect/test_destination_policy.cpp`: address classification, the
  allowance parser, and the native client refusing loopback by literal and by
  name before connecting, connecting when the class is allowed or the request is
  `Any`, and refusing a redirect hop to the metadata address.
- `tests/solutions/test_action_integration.cpp`:
  `DeliveryRefusesALoopbackEndpointTheNodeDoesNotAllow` and
  `ReportDeliveryRefusesAPrivateOrMetadataEndpoint` (nothing connects to the
  listener; the task dead-letters with the refusal as its error).

## Not covered here

- Binding a delivery credential to an administrator-approved connector with a
  fixed endpoint, requiring HTTPS, and reporting coarse delivery outcomes
  instead of the peer's status and body: tracked separately (see #1394's
  closing comment).
- Per-principal egress scoping: [#1323](https://github.com/oramasearch/zetta/issues/1323).
