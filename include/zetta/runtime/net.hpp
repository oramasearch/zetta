#pragma once

// Networking slice of the runtime seam (Corosio).
//
// Included ONLY by the networking adapter (src/core/cluster.cpp) — never by a
// public header — so Corosio stays out of the rest of zetta-core and the GUI.
// Like runtime.hpp, this is the one place that names the backend; swapping it
// (or moving to Boost.Asio) is a change here.
#include <boost/capy/buffers.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>

namespace zetta::runtime::net {

using io_context = boost::corosio::io_context;
using tcp_acceptor = boost::corosio::tcp_acceptor;
using tcp_socket = boost::corosio::tcp_socket;
using endpoint = boost::corosio::endpoint;
using ipv4_address = boost::corosio::ipv4_address;
using timer = boost::corosio::timer;

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
