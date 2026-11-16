// SPDX-License-Identifier: MIT

#include "zetta/signals.hpp"

#include <atomic>
#include <csignal>

namespace zetta {
namespace {

// Set from a signal handler, polled by the run loops. Only async-signal-safe
// operations belong in a handler — an atomic store is the canonical move.
std::atomic<bool> g_stop_requested{false};

void on_signal(int /*signum*/) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

}  // namespace

void install_signal_handlers() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

bool stop_requested() {
    return g_stop_requested.load(std::memory_order_relaxed);
}

}  // namespace zetta
