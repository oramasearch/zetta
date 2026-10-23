#pragma once

// Networking slice of the runtime seam (Corosio).
//
// Included ONLY by the three networking adapters — never by a public header —
// so Corosio stays out of the rest of zetta-core and the GUI:
//
//   * `src/core/cluster/cluster.cpp` — INBOUND/peer traffic: the listener, the
//     dialer, and the mTLS peer transport (#466).
//   * `src/core/api/native_http_transport.cpp` — OUTBOUND client traffic: the
//     native coroutine HTTPS client behind `connect::HttpTransport` (#827).
//     Compiled into `zetta-api`, not `zetta-core`, because it also needs
//     Boost.HTTP for framing.
//   * `src/core/api/mysql_protocol.cpp` — OUTBOUND MySQL wire traffic: the TCP
//     and TLS-upgrade driver for the native MySQL connector (#895), whose
//     protocol is a sans-I/O session in zetta-core (connect/mysql_client.hpp).
//     Also in `zetta-api`.
//
// Like runtime.hpp, this is the one place that names the backend; swapping it
// (or moving to Boost.Asio) is a change here.
#include <boost/capy/buffers.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/corosio/wait_type.hpp>

namespace zetta::runtime::net {

using io_context = boost::corosio::io_context;
using tcp_acceptor = boost::corosio::tcp_acceptor;
using tcp_socket = boost::corosio::tcp_socket;
using endpoint = boost::corosio::endpoint;
using ipv4_address = boost::corosio::ipv4_address;
using timer = boost::corosio::timer;

// Async DNS resolution (getaddrinfo behind the coroutine seam), so the peer
// dialer can accept a hostname seed — e.g. a Kubernetes StatefulSet per-pod
// address `foo-0.foo-headless.ns.svc.cluster.local:7777` — and connect to the
// address(es) it resolves to, re-resolving on every dial (pod IPs are
// ephemeral). See src/core/cluster/cluster.cpp dial_seed (#785).
using resolver = boost::corosio::resolver;
using resolver_results = boost::corosio::resolver_results;

// Socket readiness selector for `tcp_socket::wait()` — await raw-socket
// readability without consuming bytes. Used by the TLS read pump to wait for
// data off the per-link I/O lock, so a read and a write never drive one SSL
// object concurrently while writes still flow (cluster.cpp, #786).
using wait_type = boost::corosio::wait_type;

// SO_REUSEADDR on the cluster listener (#738). Set on the acceptor between
// open() and bind() (open() deliberately does NOT set it) so a restarted node
// can rebind its exact --listen port while the previous process's accepted
// sockets are still lingering in TIME_WAIT — instead of the OS refusing the
// bind and the node drifting to the next port.
using reuse_address = boost::corosio::socket_option::reuse_address;

// TLS on the cluster transport (CTL-2, #466): the backend-agnostic context +
// stream surface, and the OpenSSL implementation the build links.
using tls_context = boost::corosio::tls_context;
using tls_stream = boost::corosio::tls_stream;
using tls_verify_mode = boost::corosio::tls_verify_mode;
using tls_version = boost::corosio::tls_version;
using tls_file_format = boost::corosio::tls_file_format;
using openssl_stream = boost::corosio::openssl_stream;

using boost::capy::any_stream;
using boost::capy::const_buffer;
using boost::capy::mutable_buffer;
using boost::capy::write;
using boost::corosio::parse_ipv4_address;

}  // namespace zetta::runtime::net
