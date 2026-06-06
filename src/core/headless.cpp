#include "zetta/headless.hpp"

#include "zetta/node.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace zetta {
namespace {

// Set from a signal handler, polled by the loop. Only async-signal-safe
// operations belong in the handler — a flag store is the canonical move.
std::atomic<bool> g_stop_requested{false};

void on_signal(int /*signum*/) { g_stop_requested.store(true); }

}  // namespace

int run_headless(Node& node) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("zetta: headless mode — Ctrl-C to stop\n");
    std::fflush(stdout);

    using namespace std::chrono_literals;
    constexpr auto kTickInterval = 100ms;  // 10 Hz heartbeat
    constexpr int kTicksPerLog = 10;       // log status ~once per second

    int since_log = 0;
    while (!g_stop_requested.load()) {
        node.tick();
        if (++since_log >= kTicksPerLog) {
            since_log = 0;
            std::printf("zetta: %s\n", node.status_line().c_str());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(kTickInterval);
    }

    std::printf("\nzetta: signal received, shutting down\n");
    std::fflush(stdout);
    return 0;
}

}  // namespace zetta
