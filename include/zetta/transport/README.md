# Transport interface

`credentials.hpp` exposes the standard-only PEM value type. `security.hpp`
exposes file loading and construction of a mutual TLS 1.3-only Corosio context,
with the peer certificate required or verified when sent, and an anonymous
client context for enrolling (#1513).
`framed_stream.hpp` exposes the connected,
newline-framed Corosio stream. The umbrella include is `zetta/transport.hpp`.
See [the transport library documentation](../../../docs/cluster/transport-library.md).
