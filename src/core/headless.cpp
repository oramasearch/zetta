#include "zetta/headless.hpp"

#include "zetta/node.hpp"
#include "zetta/signals.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

namespace zetta {

int run_headless(Node& node) {
    install_signal_handlers();

    std::printf("zetta: headless mode — Ctrl-C to stop\n");
    std::fflush(stdout);

    using namespace std::chrono_literals;
    constexpr auto kTickInterval = 100ms;  // 10 Hz heartbeat
    constexpr int kTicksPerLog = 10;       // log status ~once per second

    int since_log = 0;
    while (!stop_requested()) {
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
