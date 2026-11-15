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
| `actor` | who the connection was for (#1323): the run's principal for a graph step, the caller for a capability, `local` for the trusted session, `node` only for the node's own work |
| `target` | `egress:<host>:<port>` |
| `allowed` | `false` |
| `detail` | the reason above, and `(run <id>)` when a graph step made the connection |
| `surface` | the surface the caller came through (#1328), carried on the denial (`HttpRequest::on_behalf_of_surface`) because the row is recorded on the client's thread; `internal` when none was declared |

**Attribution (#1323).** Each graph step runs inside a `connect::OutboundCallerScope` naming the
run's principal and the run (`pipeline/executor.cpp`); a capability call already declares its actor
(`governance::AuditActorScope`). The native HTTP client copies whichever is innermost onto the
request on the caller's thread (`HttpRequest::on_behalf_of`, `on_behalf_of_run`; never sent to the
destination) and the dialler reports it with any refusal. Work a step hands to another thread
(a compute job's worker, the distributed fetch on a peer) carries no scope yet and is attributed to
`node`.

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

The operator sets `ZETTA_EGRESS_ALLOW=*` on the Zettas it runs for a Lake, so the namespace's
NetworkSandbox stays the one place those destinations are declared and the engine does not refuse
what the workspace allows. A node run any other way (on-prem, a laptop, the desktop app, a VM, an
`OramaZettaCluster` outside a Lake) has no such layer, and this policy is what denies its egress.

## Limits

- **The allowlist is per node; what a principal may reach is narrowed by its grants.** The
  allowlist belongs to the running node, and a refusal now names the principal and run it was for
  (above). What one principal may reach is narrowed where its outbound requests come from: a saved
  connection only by its owner or a `connections:use` grant on it (#1339), an inline connector only
  with `create_connection`, a delivery's credential only to an administrator-approved origin (#1453),
  and each graph step only with the right its kind exercises (#843). A per-principal allowlist in the
  dialler itself is not built: [#1323](https://github.com/oramasearch/zetta/issues/1323) carries the
  design.
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
- `tests/connect/test_native_http_transport.cpp`: an outside address refused by default, an unlisted
  name refused before DNS, an allowlisted name dialled, loopback allowed, and a redirect from an
  allowed host to an outside one refused.
