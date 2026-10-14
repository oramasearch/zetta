#pragma once

// Newline-framed byte streams over Corosio, optionally protected by the shared
// mutual-TLS 1.3 context (#1283). See docs/cluster/transport-library.md.

#include <boost/capy/task.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace zetta::transport {

enum class HandshakeRole { Client, Server };

struct FramedStreamOptions {
    // Bounds one frame so an untrusted peer cannot grow the receive buffer
    // indefinitely. The default preserves Zetta's existing 64 MiB line limit.
    std::size_t maximum_frame_bytes = 64 * 1024 * 1024;
};

// One connected Corosio TCP socket exposed as newline-delimited frames. A frame
// is returned without its trailing newline. Payloads containing newlines must
// be encoded by the application (Zetta uses base64 for binary protocol fields).
//
// Exactly one read coroutine and one write coroutine may be active at once.
// Plain TCP remains full duplex. For TLS, the class serializes OpenSSL reads
// and writes while waiting for socket readability outside the lock, preserving
// full-duplex progress without concurrently driving one SSL object.
class FramedStream {
   public:
    explicit FramedStream(boost::corosio::tcp_socket socket, FramedStreamOptions options = {});
    FramedStream(boost::corosio::tcp_socket socket, boost::corosio::tls_context tls_context,
                 FramedStreamOptions options = {});
    ~FramedStream();

    FramedStream(const FramedStream&) = delete;
    FramedStream& operator=(const FramedStream&) = delete;
    FramedStream(FramedStream&&) noexcept;
    FramedStream& operator=(FramedStream&&) noexcept;

    // Complete the TLS handshake. A plaintext stream returns success without
    // touching the socket.
    boost::capy::task<std::error_code> handshake(HandshakeRole role);

    // Empty optional means EOF, an I/O failure, or a frame over the configured
    // bound. write_frame returns false on a closed or broken connection.
    boost::capy::task<std::optional<std::string>> read_frame();
    boost::capy::task<bool> write_frame(std::string frame);

    bool secure() const;
    void close();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zetta::transport
