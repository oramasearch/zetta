// SPDX-License-Identifier: MIT

#include "zetta/headless.hpp"

#include "zetta/cluster/cluster.hpp"
#include "zetta/node.hpp"
#include "zetta/signals.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace zetta {
namespace {

// A short id for logs — the UUID's first segment.
std::string short_id(const std::string& node_id) {
    return node_id.substr(0, node_id.find('-'));
}

// One node's replication state, e.g. "self entries=2 ver=3f9c..." or
// "follower entries=2 ver=3f9c in-sync 0.5s" / "... syncing 1.2s".
std::string replication_summary(const zetta::cluster::NodeStatus& node) {
    std::string line = node.is_self ? "self" : to_string(node.role);
    if (!node.replication.known) return line + " (no status yet)";
    line += " entries=" + std::to_string(node.replication.entries);
    line += " ver=" + node.replication.version;
    if (!node.is_self) {
        line += node.replication.in_sync ? " in-sync" : " syncing";
        char ago[32];
        std::snprintf(ago, sizeof ago, " %.1fs",
                      node.replication.seconds_since_sync);
        line += ago;
    }
    return line;
}

// A signature that changes whenever the node set, sync state, or versions move —
// so we only reprint the detailed table when something actually changed.
std::string nodes_signature(const std::vector<zetta::cluster::NodeStatus>& nodes) {
    std::string signature;
    for (const zetta::cluster::NodeStatus& node : nodes) {
        signature += node.node_id + ":" + node.replication.version + ":" +
                     (node.replication.in_sync ? "1" : "0") + ";";
    }
    return signature;
}

void print_nodes(const std::vector<zetta::cluster::NodeStatus>& nodes) {
    std::size_t in_sync = 0;
    for (const zetta::cluster::NodeStatus& node : nodes) {
        if (node.is_self || node.replication.in_sync) ++in_sync;
    }
    std::printf("zetta: nodes (%zu, %zu in sync):\n", nodes.size(), in_sync);
    for (const zetta::cluster::NodeStatus& node : nodes) {
        std::printf("zetta:   %c %s  %-22s %s\n", node.is_self ? '*' : ' ',
                    short_id(node.node_id).c_str(), node.address.c_str(),
                    replication_summary(node).c_str());
    }
}

}  // namespace

int run_headless(Node& node, const zetta::cluster::Cluster& cluster,
                 const std::function<void()>& on_tick,
                 const std::function<void(std::chrono::milliseconds)>& wait_for_work) {
    install_signal_handlers();

    // The cluster banner (node id, listen addresses, join command) is printed
    // by main() right after start(), for both modes.
    std::printf("zetta: headless mode — Ctrl-C to stop\n");
    std::fflush(stdout);

    using namespace std::chrono_literals;
    constexpr auto kTickInterval = 100ms;  // 10 Hz engine tick + API drain
    constexpr int kChecksPerTick = 10;     // re-check membership ~once per second
    // Emit the status heartbeat only every ~30s, not every second: the per-second
    // line (`running — up Ns, N ticks | cluster: N node(s)`) flooded the log on a
    // long-running headless node (#788). Membership/sync changes still print
    // promptly (within ~1s) — a change is surfaced the moment it's observed, the
    // heartbeat is just the periodic "still alive". The one-time startup banner
    // (node id / listen addresses / join command) is printed by main() and stays.
    constexpr int kChecksPerHeartbeat = 30;  // periodic heartbeat ~every 30s

    int since_check = 0;
    int since_heartbeat = 0;
    std::string last_signature;
    // Tick immediately on the first iteration, then rate-limit tick() (+ the
    // membership/heartbeat checks) to ~10 Hz while draining the API queue on
    // every wake (#731) — so requests don't wait for the next timed tick.
    auto last_tick = std::chrono::steady_clock::now() - kTickInterval;
    while (!stop_requested()) {
        // Drain the API queue every wake — the moment a request arrives, not on
        // the next timed tick (the #731 ~100 ms/request floor / ~10 req/s ceiling).
        if (on_tick) on_tick();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick >= kTickInterval) {
            last_tick = now;
            node.tick();
            if (++since_check >= kChecksPerTick) {
                since_check = 0;
                const std::vector<zetta::cluster::NodeStatus> nodes = cluster.nodes();
                std::string signature = nodes_signature(nodes);
                const bool changed = signature != last_signature;
                const bool heartbeat_due = ++since_heartbeat >= kChecksPerHeartbeat;
                // Print on a membership/sync change (surfaced promptly) or on the
                // periodic heartbeat — never every second.
                if (changed || heartbeat_due) {
                    since_heartbeat = 0;
                    std::printf("zetta: %s | cluster: %zu node(s)\n",
                                node.status_line().c_str(), nodes.size());
                    if (changed) {
                        last_signature = std::move(signature);
                        print_nodes(nodes);  // the full table only when it changed
                    }
                    std::fflush(stdout);
                }
            }
        }
        // Wait for the next request (wakes immediately on enqueue) or up to the
        // remaining tick interval; a plain sleep only when no API server is wired.
        const auto since_tick = std::chrono::steady_clock::now() - last_tick;
        const auto idle = since_tick < kTickInterval
                              ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                    kTickInterval - since_tick)
                              : std::chrono::milliseconds(0);
        if (wait_for_work)
            wait_for_work(idle);
        else
            std::this_thread::sleep_for(idle);
    }

    std::printf("\nzetta: signal received, shutting down\n");
    std::fflush(stdout);
    return 0;
}

}  // namespace zetta
