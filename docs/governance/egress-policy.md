# Egress policy: outbound network is denied by default (CTL-4)

A zetta node may connect only to **loopback** and to the outside destinations its operator puts on
an **allowlist**. Everything else is refused before a socket is opened, and before the name is
looked up in DNS. Each refusal is written to the audit trail as `egress_denied`.

This is the control for [`CTL-4`](https://github.com/oramasearch/zetta/issues/75): data egress via a
graph step or extension (THR-5) and *no data egress by default* (NFR-1). Its verification under
packet capture is [`TC-2`](https://github.com/oramasearch/zetta/issues/249)
([test script](../testing/test-scripts/tc-2-no-egress-capture.md), Part 2).

## Why one check covers every path

A zetta node opens outbound connections in exactly two places (`include/zetta/runtime/net.hpp`
names both, and no other translation unit may name the networking backend):

| Dialler | What uses it | Policy |
| --- | --- | --- |
| The cluster's peer links (`src/core/cluster/cluster.cpp`) | Other nodes of the same cluster, over mutual TLS with the cluster's own CA (CTL-2) | Not egress: a peer must prove the cluster's join token, so it is the node's own cluster |
| The native HTTP client (`src/core/api/native_http_transport.cpp`) behind `connect::HttpTransport` | Connector queries and distributed ingest, solution action integrations (webhooks, CRM plugins), model-weight downloads, the Clerk JWKS fetch, and anything a graph step reaches | **Checked here**, before every connection, including each redirect hop |

zetta runs no foreign code: SQL, simulated Python and natural language are re-implemented, not
executed (`include/zetta/input/python_frontend.hpp`). A graph step therefore has no socket of its own;
anything it sends out goes through the HTTP client, and so through the policy.

## Configuring the allowlist

| Where | How |
| --- | --- |
| Command line | `--egress-allow LIST`, repeatable |
| Environment | `ZETTA_EGRESS_ALLOW=LIST` (used only when no `--egress-allow` is given) |

`LIST` is comma- or space-separated. Each entry is one destination, optionally followed by `:port`
(no port means any port):

| Entry | Admits |
| --- | --- |
| `api.example.com` | exactly that host name (case-insensitive; a trailing dot is ignored) |
| `*.snowflakecomputing.com` | any name under that domain, not the domain itself |
| `203.0.113.7`, `[2001:db8::1]` | exactly that address, however the URL spells it |
| `10.0.0.0/8`, `[2001:db8::/32]` | any address in that range: a name is resolved, and only its addresses inside the range are dialled |
| `*` | every destination: **turns the control off** |
| `*:443` | every destination, on port 443 only |

An IPv6 entry with a port must be bracketed: `[2001:db8::1]:443`. A malformed entry stops the node at
startup with the entry and the reason; a typo never silently widens or narrows the allowlist.

The node prints the policy it runs with at startup:

```
zetta: egress deny (loopback only)
zetta: egress allow *.snowflakecomputing.com:443,hooks.example.com:443
```

### Added automatically

- **The Clerk issuer.** When `--clerk-issuer` / `ZETTA_CLERK_ISSUER` is set, its host and port are
  added, because the auth gate fetches that issuer's JWKS. Without an issuer the node is bearer-only
  and needs no outside access to authenticate anyone.
- **`zetta pull`** (the command a person runs to download open weights) may reach `huggingface.co`,
  `*.huggingface.co` and `*.hf.co` on 443, plus whatever `ZETTA_EGRESS_ALLOW` adds. It is a separate
  process and never a running node.

### Loopback is not egress

`localhost`, `*.localhost`, `127.0.0.0/8` and `::1` (and IPv4-mapped loopback) are reachable under
every policy. Traffic to them never leaves the machine, or in Kubernetes the pod, the node runs on; a
sidecar on loopback is part of the node's own deployment.

## What a refusal looks like

The caller receives a transport error, never an HTTP status:

```
egress denied: hooks.example.com:443 is not on this node's egress allowlist (add it with --egress-allow or ZETTA_EGRESS_ALLOW; see docs/governance/egress-policy.md)
```

A connector query, an action delivery or a model download fails with that reason. The audit trail
gets one `egress_denied` event per refused connection:

| Field | Value |
| --- | --- |
| `action` | `egress_denied` |
| `actor` | `node` |
| `target` | `egress:<host>:<port>` |
| `allowed` | `false` |
| `detail` | the reason above |

A name refused by the allowlist is never resolved, so the refusal sends no DNS query either: a DNS
lookup can carry data out on its own.

## Under Kubernetes: two layers

When the Orama operator runs a node inside a Lake (the Lake's own Zetta, every workspace Zetta and
every managed Zetta service), the workspace namespace already has a default-deny NetworkSandbox: the
pods may reach cluster DNS, their Lake's API and the destinations declared as Lake Connections and
EgressRules, and the public internet on 443 only when the workspace's `internetEgress` override is on
(`orama-operator` `internal/controller/lake_workspaces.go`, `networksandbox_controller.go`,
`egressrule_controller.go`). That layer is enforced by the cluster's network policy, outside the
process.

The operator renders `ZETTA_EGRESS_ALLOW` for every Zetta a Lake owns from what its namespace's
NetworkSandbox allows it ([orama-operator#724](https://github.com/oramasearch/orama-operator/issues/724),
its `docs/infrastructure/egress-rules.md`), so the NetworkSandbox stays the one place those
destinations are declared and the engine refuses what the network policy refuses. A node run any other way (on-prem, a laptop, the desktop app, a VM, an
`OramaZettaCluster` outside a Lake) has no such layer, and this policy is what denies its egress.

## The workspace egress gateway

Inside a Lake workspace an `EgressRule` with a DNS name never opens a direct path to the destination.
The operator's network policy lets the rule's source pods reach only the workspace's egress gateway
(zetta-tunnel's `egress` role, the Service `zetta-tunnel-egress.<namespace>.svc.cluster.local`),
which authorizes the source address, the exact name and the port, resolves the name itself, refuses
the whole lookup when any answer is not a public unicast address, and forwards the bytes (zetta-tunnel
[`docs/egress.md`](https://github.com/oramasearch/zetta-tunnel/blob/dev/docs/egress.md)). A node that
dialled the name directly would be refused by the network
([#1331](https://github.com/oramasearch/zetta/issues/1331)).

| Where | How |
| --- | --- |
| Command line | `--egress-gateway HOST[:PORT]` (port default `7445`), and `--egress-gateway-route LIST`, repeatable |
| Environment | `ZETTA_EGRESS_GATEWAY`, `ZETTA_EGRESS_GATEWAY_ROUTES` (each used only when its flag is not given) |

A **route** uses the allowlist's syntax restricted to names: `api.example.com` or `*.example.com`,
each optionally `:port`. A route that is `*`, an address or an address range stops the node at
startup: those destinations are enforced by the network directly and never go through the gateway.
Routes without a gateway, or a gateway without routes, stop the node too.

For each connection, after the allowlist has admitted the **destination** name:

- A destination that is a DNS name and matches a route is reached through the gateway. **HTTPS on
  443** connects to the gateway and sends its ordinary TLS ClientHello with the destination as SNI:
  TLS stays end to end and the certificate is checked against the destination name, so the gateway
  never sees the request in clear. **Any other port** sends the explicit preface
  `ZTEG1 <name> <port>\n` first, then TLS (`https://`) or plaintext (`http://`).
- Everything else is dialled directly, as before: IP literals, loopback, and names no route matches
  (in-cluster Services named by Connections, address-range rules, `*:443` for `internetEgress`).
- Redirect hops are new connections and take the same decision.

What it does not change:

- **The allowlist decides first.** A routed name the allowlist does not admit is refused, audited
  as `egress_denied`, and the gateway is never dialled. The gateway address is configuration, not an
  allowlist entry, so setting it widens nothing.
- **The node does not resolve a routed name.** The gateway resolves it and refuses any non-public
  answer, which is at least as strict as the [destination policy](destination-policy.md) for
  caller-chosen URLs and connectors. A connector to an approved private address is never a route
  (it is an in-cluster name or an address).

The startup log names the gateway:

```
zetta: egress gateway zetta-tunnel-egress.ws-1.svc.cluster.local:7445 for api.example.com:443
```

**Re-read from files, without a restart** ([#1517](https://github.com/oramasearch/zetta/issues/1517)).
When `ZETTA_EGRESS_ALLOW_FILE` names the allowlist file (#1332) and no `--egress-gateway` flag is
given, the node also reads `gateway` and `gatewayRoutes` in that file's directory: the keys the
operator writes into the cluster's egress ConfigMap. `gateway` holds the address, `gatewayRoutes`
the routes (one per line or comma separated; `#` starts a comment). They are re-read every 2
seconds, so a name rule added or removed reaches the running node without a restart.

- While **neither** file exists, `ZETTA_EGRESS_GATEWAY` / `ZETTA_EGRESS_GATEWAY_ROUTES` apply.
- A replacement that is malformed or unreadable, or routes without a gateway, is refused and
  logged once; the last good gateway stays. Malformed at startup stops the node.
- The `--egress-gateway` / `--egress-gateway-route` flags pin the gateway and turn the files off.

```
zetta: egress gateway re-reading /etc/zetta/egress/{gateway,gatewayRoutes}
zetta: egress gateway reloaded from /etc/zetta/egress: zetta-tunnel-egress.ws-1.svc.cluster.local:7445 for api.example.com:443
```

**Under the operator** the gateway variables are the operator's to render, next to
`ZETTA_EGRESS_ALLOW`: `ZETTA_EGRESS_GATEWAY=zetta-tunnel-egress.<namespace>.svc.cluster.local:7445`
and `ZETTA_EGRESS_GATEWAY_ROUTES` set to the name rules' `<dnsName>:<port>` entries, on the workspace
Zettas whose namespace has a name rule. Until it does, a node has no gateway and dials names
directly.

## Limits

- **Per node, not per principal or per run.** The allowlist belongs to the running node. Scoping what
  one graph step may reach by the principal of its run is the deny-by-default authority over graph
  execution in [#843](https://github.com/oramasearch/zetta/issues/843). The refusal is audited with
  actor `node` because the dialler does not know which run asked.
- **Subprocesses are outside it.** The cloud provisioner's read-only `CommandRunner` calls run a
  provider CLI as a separate process; those calls are consent-gated
  ([../api/consent.md](../api/consent.md)) and permission-gated
  (`include/zetta/cloud/cloud_provider.hpp`), not checked here.
- **A name, not its answer.** A host entry trusts what the name resolves to. Use an address range
  when the destination must be pinned to addresses.

## Code and tests

- `include/zetta/connect/egress_policy.hpp`, `src/core/connect/egress_policy.cpp`: the policy,
  its parser, the process-wide installed policy and the denial observer.
- `src/core/api/native_http_transport.cpp` (`perform_url`): the check before DNS and before each
  connect, for every hop.
- `src/main.cpp`: `--egress-allow` / `ZETTA_EGRESS_ALLOW`, the Clerk issuer, the startup line, the
  `egress_denied` audit events, and `zetta pull`'s allowance.
- `tests/connect/test_egress_policy.cpp`: parsing, every entry kind, ports, loopback, ranges, malformed
  entries, the observer.
- `include/zetta/connect/egress_gateway.hpp`, `src/core/connect/egress_gateway.cpp`: the gateway
  address and routes, which destinations are routed, the `ZTEG1` preface, the installed gateway.
- `tests/connect/test_egress_gateway.cpp`: the address and route parser, names-only routes, what is
  never routed, the preface, and the gateway files: the environment while they are absent, a change
  applied on the next read, a malformed replacement refused once with the last good gateway kept,
  and a malformed file at startup refused.
- `src/main.cpp` (`EgressGatewayFilesReloader`): the 2-second re-read of the gateway files (#1517).
- `tests/connect/test_native_http_transport.cpp`: an outside address refused by default, an unlisted
  name refused before DNS, an allowlisted name dialled, loopback allowed, and a redirect from an
  allowed host to an outside one refused; through a stand-in gateway, a routed name reached with the
  preface without being resolved, HTTPS on 443 sent by SNI with no preface, the allowlist refusing a
  routed name before the gateway is dialled, an unrouted name dialled directly, and a redirect hop to
  a routed name sent through the gateway.
