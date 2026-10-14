#include "zetta/transport/framed_stream.hpp"

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/async_mutex.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/wait_type.hpp>
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
    capy::async_mutex io_lock;
    bool first_tls_read = true;
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
    char plaintext_buffer[4096];
    char tls_buffer[16384];
    for (;;) {
        if (const std::size_t newline = impl_->pending.find('\n'); newline != std::string::npos) {
            std::string frame = impl_->pending.substr(0, newline);
            impl_->pending.erase(0, newline + 1);
            co_return frame;
        }
        if (impl_->pending.size() > impl_->options.maximum_frame_bytes) {
            co_return std::nullopt;
        }

        if (!impl_->tls) {
            capy::mutable_buffer chunk(plaintext_buffer, sizeof plaintext_buffer);
            auto [error, count] = co_await impl_->stream.read_some(std::span(&chunk, 1));
            if (error || count == 0) {
                co_return std::nullopt;
            }
            impl_->pending.append(plaintext_buffer, count);
            continue;
        }

        // The TLS handshake may already have pulled the peer's first application
        // record off the raw socket. Read once without a readiness wait so those
        // buffered bytes cannot be stranded inside the TLS engine. Later reads
        // wait before taking the lock, which leaves the write pump free while the
        // connection is idle.
        if (!impl_->first_tls_read) {
            if (auto [wait_error] = co_await impl_->socket.wait(corosio::wait_type::read);
                wait_error) {
                co_return std::nullopt;
            }
        }
        auto [lock_error, guard] = co_await impl_->io_lock.scoped_lock();
        if (lock_error) {
            co_return std::nullopt;
        }
        impl_->first_tls_read = false;
        for (;;) {
            capy::mutable_buffer chunk(tls_buffer, sizeof tls_buffer);
            auto [read_error, count] = co_await impl_->stream.read_some(std::span(&chunk, 1));
            if (read_error || count == 0) {
                co_return std::nullopt;
            }
            impl_->pending.append(tls_buffer, count);
            if (impl_->pending.find('\n') != std::string::npos) {
                break;
            }
            if (count < sizeof tls_buffer) {
                break;
            }
        }
    }
}

capy::task<bool> FramedStream::write_frame(std::string frame) {
    frame.push_back('\n');
    if (!impl_->tls) {
        auto [error, written] =
            co_await capy::write(impl_->stream, capy::const_buffer(frame.data(), frame.size()));
        co_return !error && written == frame.size();
    }

    auto [lock_error, guard] = co_await impl_->io_lock.scoped_lock();
    if (lock_error) {
        co_return false;
    }
    auto [error, written] =
        co_await capy::write(impl_->stream, capy::const_buffer(frame.data(), frame.size()));
    co_return !error && written == frame.size();
}

bool FramedStream::secure() const {
    return impl_->tls != nullptr;
}

void FramedStream::close() {
    impl_->socket.close();
}

}  // namespace zetta::transport
