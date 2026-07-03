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
                 const std::function<void()>& on_tick) {
    install_signal_handlers();

    // The cluster banner (node id, listen addresses, join command) is printed
    // by main() right after start(), for both modes.
    std::printf("zetta: headless mode — Ctrl-C to stop\n");
    std::fflush(stdout);

    using namespace std::chrono_literals;
    constexpr auto kTickInterval = 100ms;  // 10 Hz heartbeat
    constexpr int kTicksPerLog = 10;       // log status ~once per second

    int since_log = 0;
    std::string last_signature;
    while (!stop_requested()) {
        node.tick();
        if (on_tick) on_tick();
        if (++since_log >= kTicksPerLog) {
            since_log = 0;
            const std::vector<zetta::cluster::NodeStatus> nodes = cluster.nodes();
            // Always a one-line heartbeat; the full table only when it changes
            // (a node joined/left or a sync state flipped).
            std::printf("zetta: %s | cluster: %zu node(s)\n",
                        node.status_line().c_str(), nodes.size());
            if (std::string signature = nodes_signature(nodes);
                signature != last_signature) {
                last_signature = std::move(signature);
                print_nodes(nodes);
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(kTickInterval);
    }

    std::printf("\nzetta: signal received, shutting down\n");
    std::fflush(stdout);
    return 0;
}

}  // namespace zetta
