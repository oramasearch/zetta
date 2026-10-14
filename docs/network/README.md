# network — workspace network access and records

The **network** domain owns scoped app credentials and the durable records a
workspace's people inspect: declared-connection history, tunnel-grant history,
developer tunnel sessions, and decisions/activity from zetta-tunnel's gateways
and mesh. Desired state that the operator enforces remains in Lake's Postgres
control plane.

| Page | What it covers |
|---|---|
| [scoped-app-access.md](scoped-app-access.md) | One hashed, revocable credential per app connection, least-privilege table/topic enforcement, lifecycle, and audit (#1280). |
| [network-records.md](network-records.md) | The four retained record collections, validated producer JSON, zetta-tunnel writes, Lake reads, permissions, and workspace isolation (#1281). |
