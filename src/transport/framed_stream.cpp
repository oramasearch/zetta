// SPDX-License-Identifier: MIT

#include "zetta/transport/framed_stream.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <span>
#include <utility>

namespace zetta::transport {

namespace capy = boost::capy;
namespace corosio = boost::corosio;

struct FramedStream::Impl {
    explicit Impl(corosio::tcp_socket accepted, FramedStreamOptions stream_options)
        : socket(std::move(accepted)), stream(&socket), options(stream_options) {
    }

    Impl(corosio::tcp_socket accepted, corosio::tls_context context,
         FramedStreamOptions stream_options)
        : socket(std::move(accepted)),
          tls(std::make_unique<corosio::openssl_stream>(&socket, context)),
          stream(tls.get()),
          options(stream_options) {
    }

    corosio::tcp_socket socket;
    std::unique_ptr<corosio::openssl_stream> tls;
    capy::any_stream stream;
    FramedStreamOptions options;
    std::string pending;
};

FramedStream::FramedStream(corosio::tcp_socket socket, FramedStreamOptions options)
    : impl_(std::make_unique<Impl>(std::move(socket), options)) {
}

FramedStream::FramedStream(corosio::tcp_socket socket, corosio::tls_context tls_context,
                           FramedStreamOptions options)
    : impl_(std::make_unique<Impl>(std::move(socket), std::move(tls_context), options)) {
}

FramedStream::~FramedStream() = default;
FramedStream::FramedStream(FramedStream&&) noexcept = default;
FramedStream& FramedStream::operator=(FramedStream&&) noexcept = default;

capy::task<std::error_code> FramedStream::handshake(HandshakeRole role) {
    if (!impl_->tls) {
        co_return std::error_code{};
    }
    auto [error] = co_await impl_->tls->handshake(
        role == HandshakeRole::Client ? corosio::tls_stream::client : corosio::tls_stream::server);
    co_return error;
}

capy::task<std::optional<std::string>> FramedStream::read_frame() {
    // One TLS record carries at most 16 KiB of plaintext, so a buffer this size
    // takes a whole record per call. Corosio's openssl_stream returns bytes
    // already buffered in the TLS engine before it reads the socket again, so
    // nothing received is left waiting for more traffic to arrive.
    char buffer[16384];
    for (;;) {
        if (const std::size_t newline = impl_->pending.find('\n'); newline != std::string::npos) {
            std::string frame = impl_->pending.substr(0, newline);
            impl_->pending.erase(0, newline + 1);
            co_return frame;
        }
        if (impl_->pending.size() > impl_->options.maximum_frame_bytes) {
            co_return std::nullopt;
        }
        capy::mutable_buffer chunk(buffer, sizeof buffer);
        auto [error, count] = co_await impl_->stream.read_some(std::span(&chunk, 1));
        if (error || count == 0) {
            co_return std::nullopt;
        }
        impl_->pending.append(buffer, count);
    }
}

void FramedStream::set_maximum_frame_bytes(std::size_t maximum_frame_bytes) {
    impl_->options.maximum_frame_bytes = maximum_frame_bytes;
}

capy::task<bool> FramedStream::write_frame(std::string frame) {
    frame.push_back('\n');
    auto [error, written] =
        co_await capy::write(impl_->stream, capy::const_buffer(frame.data(), frame.size()));
    co_return !error && written == frame.size();
}

bool FramedStream::secure() const {
    return impl_->tls != nullptr;
}

std::vector<unsigned char> FramedStream::peer_certificate_der() const {
    if (!impl_->tls) {
        return {};
    }
    return impl_->tls->peer_certificate_der();
}

void FramedStream::close() {
    impl_->socket.close();
}

}  // namespace zetta::transport
