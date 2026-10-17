# Transport implementation

This directory implements the independent `zetta_transport` /
`zetta::transport` CMake target: PEM loading and OpenSSL verification in
`security.cpp`, and newline framing plus TLS I/O coordination in
`framed_stream.cpp`. It must not depend on `zetta-core`. See
[the transport library documentation](../../docs/cluster/transport-library.md).
