// SPDX-License-Identifier: MIT

#include "zetta/connect/http_transport.hpp"

#include <mutex>
#include <utility>

namespace zetta::connect {
namespace {

// The installed default transport pair. This TU holds nothing but the registry:
// the real client is a `zetta-api` TU (src/core/api/native_http_transport.cpp)
// that installs itself at startup, because zetta-core does not link Boost.HTTP
// or Beast2 and must never see an HTTP type.
//
// A raw mutex is permitted here for the same reason it is in the cluster's
// outbox: this is the lowest-level cross-thread handoff inside a seam adapter —
// installed once on the main thread at startup, read from any thread afterwards
// — and it is invisible in the public API.
struct DefaultTransports {
    std::mutex guard;
    HttpTransport synchronous;
    AsyncHttpTransport asynchronous;
};

DefaultTransports& installed() {
    static DefaultTransports transports;
    return transports;
}

}  // namespace

void set_default_http_transport(HttpTransport synchronous,
                                AsyncHttpTransport asynchronous) {
    DefaultTransports& transports = installed();
    const std::lock_guard<std::mutex> lock(transports.guard);
    transports.synchronous = std::move(synchronous);
    transports.asynchronous = std::move(asynchronous);
}

const HttpTransport& default_http_transport() {
    DefaultTransports& transports = installed();
    const std::lock_guard<std::mutex> lock(transports.guard);
    return transports.synchronous;
}

const AsyncHttpTransport& default_async_http_transport() {
    DefaultTransports& transports = installed();
    const std::lock_guard<std::mutex> lock(transports.guard);
    return transports.asynchronous;
}

}  // namespace zetta::connect
