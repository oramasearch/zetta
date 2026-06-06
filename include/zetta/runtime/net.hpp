#pragma once

// Networking slice of the runtime seam (Corosio).
//
// Included ONLY by the networking adapter (src/core/cluster.cpp) — never by a
// public header — so Corosio stays out of the rest of zetta-core and the GUI.
// Like runtime.hpp, this is the one place that names the backend; swapping it
// (or moving to Boost.Asio) is a change here.
#include <boost/capy/buffers.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio.hpp>
#include <boost/corosio/tcp_acceptor.hpp>

namespace zetta::runtime::net {

using io_context = boost::corosio::io_context;
using tcp_acceptor = boost::corosio::tcp_acceptor;
using tcp_socket = boost::corosio::tcp_socket;
using endpoint = boost::corosio::endpoint;
using ipv4_address = boost::corosio::ipv4_address;

using boost::capy::const_buffer;
using boost::capy::mutable_buffer;
using boost::capy::write;
using boost::corosio::parse_ipv4_address;

}  // namespace zetta::runtime::net
