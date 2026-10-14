# network — workspace network records

The **network** domain owns the durable records a workspace's people inspect:
declared-connection history, tunnel-grant history, developer tunnel sessions,
and decisions/activity from zetta-tunnel's gateways and mesh. Desired state
that the operator enforces remains in Lake's Postgres control plane.

| Page | What it covers |
|---|---|
| [network-records.md](network-records.md) | The four retained record collections, validated producer JSON, zetta-tunnel writes, Lake reads, permissions, and workspace isolation (#1281). |
