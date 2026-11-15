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
| A file re-read while running | `--egress-allow-file PATH`, or `ZETTA_EGRESS_ALLOW_FILE=PATH` when no flag is given ([below](#the-allowlist-file-re-read-while-running), #1332) |

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

### The allowlist file, re-read while running

`--egress-allow-file PATH` ([#1332](https://github.com/oramasearch/zetta/issues/1332)) names a file
of further entries that the node re-reads while it runs, so a change to what it may reach takes
effect without a restart. The file uses the same entries as `LIST`, separated by commas, spaces or
line breaks; `#` starts a comment that runs to the end of its line:

```
# rendered by whoever runs the node
api.example.com:443
*.snowflakecomputing.com:443
10.20.0.0/16
```

- **What is in force** is always the static part (`--egress-allow` / `ZETTA_EGRESS_ALLOW` and the
  Clerk issuer) plus the file's last good contents. A replacement can widen or narrow the file's
  part; it never removes the static part.
- **Re-read every 2 seconds.** Each read opens the path afresh and follows symlinks, so a Kubernetes
  projected ConfigMap, which kubelet replaces by swapping its `..data` symlink, is picked up. A
  read that finds the same bytes changes nothing.
- **A bad replacement never changes the policy.** A malformed file, one that cannot be read, one
  larger than 1 MiB, one that is not a regular file, or one that was removed keeps the last good
  policy in force. It is logged on stderr and audited once per distinct state, until a well-formed
  file replaces it. To deny everything the file allowed, write an empty file (or one of only
  comments); removing it does not.
- **At startup**, a missing file adds nothing (deny, apart from the static part) and is applied once
  it appears; a malformed or unreadable file stops the node with the entry and the reason, like a
  malformed `--egress-allow`.
- **Every load and reload names the policy in force**: the startup line says which file is re-read,
  each applied replacement prints a line, and each load, replacement and refusal is an
  `egress_policy_reloaded` audit event:

```
zetta: egress allow api.example.com:443 (re-reading /etc/zetta/egress/allow)
zetta: egress reloaded from /etc/zetta/egress/allow: allow api.example.com:443,10.20.0.0/16
zetta: egress allowlist replacement refused: egress allowlist file /etc/zetta/egress/allow: egress allowlist entry 'host!': ...; the last good policy stays in force
```

| Field | Value |
| --- | --- |
| `action` | `egress_policy_reloaded` |
| `actor` | `node` |
| `target` | `egress_allow_file:<path>` |
| `allowed` | `true` when the file's policy was put in force (at startup or on a replacement), `false` when a replacement was refused |
| `detail` | `in force: <the policy>`, then the reason in parentheses for a refusal or a missing file |

A connection already open when a replacement lands is not cut; the new policy decides every
connection (and redirect hop) dialled after it.

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

The operator sets `ZETTA_EGRESS_ALLOW=*` on the Zettas it runs for a Lake, so the namespace's
NetworkSandbox stays the one place those destinations are declared and the engine does not refuse
what the workspace allows. Where the operator renders a per-Zetta allowlist from the NetworkSandbox
instead ([orama-operator#724](https://github.com/oramasearch/orama-operator/issues/724)), the
variable sits in the pod template and every rule or Connection change restarts the Zetta; projecting
a ConfigMap and passing its path as `ZETTA_EGRESS_ALLOW_FILE` stops that. That switch is an operator
change, not made here. A node run any other way (on-prem, a laptop, the desktop app, a VM, an
`OramaZettaCluster` outside a Lake) has no such layer, and this policy is what denies its egress.

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
- `include/zetta/connect/egress_policy.hpp` `EgressAllowFile`, `parse_egress_allow_file`: the
  allowlist file (#1332).
- `src/main.cpp`: `--egress-allow` / `ZETTA_EGRESS_ALLOW`, `--egress-allow-file` /
  `ZETTA_EGRESS_ALLOW_FILE` and the thread that re-reads it (`EgressAllowFileReloader`), the Clerk
  issuer, the startup line, the `egress_denied` and `egress_policy_reloaded` audit events, and
  `zetta pull`'s allowance (which does not read the file).
- `tests/connect/test_egress_policy.cpp`: parsing, every entry kind, ports, loopback, ranges, malformed
  entries, the observer; the file's comments, a missing file at startup, a malformed file at startup,
  a replacement on top of the static part, a malformed or removed replacement keeping the last good
  policy (reported once), a kubelet-style symlink swap, and a directory refused.
- `tests/connect/test_native_http_transport.cpp`: an outside address refused by default, an unlisted
  name refused before DNS, an allowlisted name dialled, loopback allowed, and a redirect from an
  allowed host to an outside one refused.
