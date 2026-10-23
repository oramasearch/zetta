# Reusable Corosio transport library

Issue [#1283](https://github.com/oramasearch/zetta/issues/1283) extracts the
networking code shared by Zetta's cluster and
[`zetta-tunnel`](https://github.com/oramasearch/zetta-tunnel): mutual TLS 1.3,
PEM credential loading and verification, and newline-framed byte streams over
Corosio. Zetta's cluster uses the library target, so fixes to certificate
validation, frame buffering, or OpenSSL read/write coordination have one owner.

## CMake consumption

Initialize the `modules/capy` and `modules/corosio` submodules recursively, make
OpenSSL 3 available to CMake, and configure the parent project with a C++26
compiler (Homebrew LLVM on macOS). A parent project chooses its compiler before
`add_subdirectory`, so Zetta cannot select Homebrew LLVM on the parent's behalf.
Then add this repository in transport-only mode:

```cmake
set(ZETTA_TRANSPORT_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(/path/to/zetta zetta-transport EXCLUDE_FROM_ALL)

target_link_libraries(my_target PRIVATE zetta::transport)
```

The concrete target name `zetta_transport` is also available. The namespaced
`zetta::transport` alias is preferred. Both propagate the C++26 requirement,
the public `include/` directory, Capy, Corosio, and Corosio's OpenSSL provider.
The target does not link `zetta-core`, SDL, ImGui, Automerge, the query engine,
or the API server. `ZETTA_TRANSPORT_ONLY` returns from this repository's CMake
configuration after creating the transport and its direct dependencies.
`BUILD_SHARED_LIBS=OFF` creates static transport, Capy, and Corosio libraries;
set it to `ON` before `add_subdirectory` to create shared libraries.

The direct third-party dependencies are Capy and Corosio under the Boost
Software License 1.0 (their vendored `LICENSE` files), and OpenSSL under the
Apache License 2.0. Consumers remain responsible for carrying the applicable
notices in their distribution.

## Public interface

Include the complete surface with:

```cpp
#include <zetta/transport.hpp>
```

Or include the focused headers:

- `zetta/transport/credentials.hpp` provides the standard-library-only
  `TlsCredentials` value used by Zetta's public cluster configuration.
- `zetta/transport/security.hpp` provides `TlsCredentialFiles`,
  `load_tls_credentials`,
  `make_mutual_tls_context`, and `certificate_common_name`.
- `zetta/transport/framed_stream.hpp` provides `FramedStream`,
  `FramedStreamOptions`, and `HandshakeRole`.

`make_mutual_tls_context` performs local checks before a connection starts: the
leaf and chain must be valid PEM, the private key must match the leaf, the leaf
must verify against the supplied CA bundle at the current time, and every CA in
the bundle is installed as a trust anchor. The resulting Corosio context uses
only TLS 1.3 and requires a verified peer certificate in both client and server
roles. It never adds system trust anchors. Every failure returns an empty
optional and a field-qualified reason.

`FramedStream` takes an already connected `boost::corosio::tcp_socket`, with an
optional context from `make_mutual_tls_context`. Call `handshake(Client)` on the
dialing side and `handshake(Server)` on the accepting side before reading or
writing frames. A frame is a byte string followed by `\n`; reads remove the
delimiter and preserve bytes after it for the next call. Applications encode a
payload that can contain a newline before passing it to the stream. Zetta's
wire protocol already base64-encodes binary fields.

Exactly one read coroutine and one write coroutine may be active on a stream,
on one executor. Both transports are full duplex: a read that is waiting for the
peer never holds back a write, and a write that is waiting for socket space
never holds back a read. For TLS this is Corosio's `openssl_stream` (our fork,
[oramasearch/corosio](https://github.com/oramasearch/corosio)): the `SSL` calls
never suspend, so a read and a write cannot drive the one `SSL` object at the
same time, and the socket I/O between them takes a read lock or a write lock,
never one lock for both. A read returns bytes already buffered in the TLS engine
before it reads the socket again, so a received frame never waits for later
traffic. The default maximum frame size is 64 MiB, which preserves the cluster's
existing limit and can be lowered per stream, either at
construction (`FramedStreamOptions`) or between reads
(`FramedStream::set_maximum_frame_bytes`). The bound is checked inside the TLS
drain loop too, so one read never buffers far past it.

After a TLS handshake, `FramedStream::peer_certificate_der()` returns the DER
certificate the peer presented (Corosio's `tls_stream::peer_certificate_der()`,
corosio#14); it is empty on a plaintext stream. `certificate_common_name(der)`
reads its subject common name, refusing bytes that are not exactly one
certificate and subjects with no common name or more than one. It does not
verify the certificate: the handshake already did, against the context's CA.

Full duplex matters because both ends of a cluster link write their whole
replicated document every gossip round. Until #835 the stream shared one lock
between its reader and writer, and the reader could hold it while waiting for
the peer. When both peers did that at once, each reader waited for a frame the
other side's writer could not send, and the link froze: no error, no dropped
connection, and no keepalive, because each writer was parked on the lock. The
nodes kept reporting their peers while one of them stopped applying `SYNC`.
`tests/transport/test_framed_stream.cpp` reproduces it on one loopback
connection: both peers write a frame larger than the socket buffers at once,
which the old stream never delivers in either direction. A second case streams
gossip-sized frames both ways and checks every one arrives intact and in order.

## Zetta cluster integration

`zetta::cluster::ClusterSecurity` extends the reusable `TlsCredentials` with
Zetta's join token and retired-token history. Token derivation, node certificate
minting, and persistence remain cluster-specific in
`src/core/cluster/security.cpp`. `Cluster::start()` passes those credentials to
`make_mutual_tls_context`; each accepted or dialed socket is then wrapped in a
`FramedStream` before any `ZETTA/1 HELLO` byte is exchanged. Once the peer's
HELLO is parsed, the cluster refuses the link unless the stated node id is the
common name of the peer's certificate (`hello_matches_peer_certificate`, #669).

The extraction changes no cluster wire format, handshake order, reconnect
logic, or plaintext behavior for embedded test clusters. The cluster reads a
peer's HELLO under a 16 KiB frame bound and restores the 64 MiB bound once the
HELLO is accepted (#771; see [cluster](cluster.md)). Existing
cluster tests exercise the library through the same public `Cluster` surface.

## Boundaries and later work

This slice adds no JSON schema and changes no cluster wire format. It does not
build zetta-tunnel, add tunnel identities, introduce QUIC, or implement scoped
Zetta access and network records; those remain in #1280, #1281, #1282, and the
zetta-tunnel issues. Length-prefixed binary framing can be added as another
explicit framing policy without changing the cluster's newline protocol.
