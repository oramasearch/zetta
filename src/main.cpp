// zetta — entry point.
//
// Parses arguments, builds a zetta::Node + a zetta::Cluster, and dispatches to
// one of two run modes. Headed (the default) renders the node in an SDL3 + Dear
// ImGui window; headless runs the same node with no UI, for servers with no
// display. All the real work lives behind zetta::Node / zetta::Cluster — main
// only picks how to drive them.

#include "zetta/cluster.hpp"
#include "zetta/headless.hpp"
#include "zetta/node.hpp"
#include "zetta/replicated_blob_store.hpp"
#include "zetta/replication.hpp"
#include "zetta/uuid.hpp"
#include "zetta/version.hpp"

#include "gui/app.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace {

enum class Mode { Headed, Headless };

struct Options {
    Mode mode = Mode::Headed;  // headed by default
    zetta::NodeConfig node;
    zetta::ClusterConfig cluster;
    bool show_help = false;
    bool bad_usage = false;
};

bool parse_uint16(const std::string& text, std::uint16_t& out) {
    unsigned value = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0 ||
        value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

void print_usage(const char* argv0) {
    const zetta::ClusterConfig defaults;
    std::printf(
        "zetta %s — a database node\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --headed            Run with the SDL3 + ImGui UI (default)\n"
        "  --headless          Run without a UI (server only)\n"
        "  --data-dir PATH     Database directory (default: %s)\n"
        "  --listen HOST:PORT  Cluster listen address (default: %s:%u)\n"
        "  --port-range N      Ports to try from --listen, for several nodes\n"
        "                      per host (default: %u)\n"
        "  --peer HOST:PORT    A node to connect to (repeatable; all equal)\n"
        "  --log-cluster       Log cluster gossip events to stderr (debugging)\n"
        "  --no-preflight      Skip the startup data integrity check\n"
        "  -h, --help          Show this help\n",
        zetta::kVersion, argv0, zetta::NodeConfig{}.data_dir.c_str(),
        defaults.listen_host.c_str(), defaults.listen_port, defaults.port_range);
}

const char* take_value(int argc, char** argv, int& i, Options& o,
                       const char* flag) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "zetta: %s needs a value\n", flag);
        o.bad_usage = true;
        return nullptr;
    }
    return argv[++i];
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            o.mode = Mode::Headless;
        } else if (arg == "--headed") {
            o.mode = Mode::Headed;
        } else if (arg == "--data-dir") {
            const char* v = take_value(argc, argv, i, o, "--data-dir");
            if (!v) break;
            o.node.data_dir = v;
        } else if (arg == "--listen") {
            const char* v = take_value(argc, argv, i, o, "--listen");
            if (!v) break;
            const std::string text = v;
            const auto colon = text.rfind(':');
            std::uint16_t port = 0;
            if (colon == std::string::npos ||
                !parse_uint16(text.substr(colon + 1), port)) {
                std::fprintf(stderr, "zetta: --listen wants HOST:PORT\n");
                o.bad_usage = true;
                break;
            }
            o.cluster.listen_host = text.substr(0, colon);
            o.cluster.listen_port = port;
        } else if (arg == "--port-range") {
            const char* v = take_value(argc, argv, i, o, "--port-range");
            if (!v) break;
            if (!parse_uint16(v, o.cluster.port_range)) {
                std::fprintf(stderr, "zetta: --port-range wants a number\n");
                o.bad_usage = true;
                break;
            }
        } else if (arg == "--peer") {
            const char* v = take_value(argc, argv, i, o, "--peer");
            if (!v) break;
            o.cluster.seeds.emplace_back(v);
        } else if (arg == "--log-cluster") {
            o.cluster.log_events = true;
        } else if (arg == "--no-preflight") {
            o.node.preflight = false;
        } else if (arg == "-h" || arg == "--help") {
            o.show_help = true;
        } else {
            std::fprintf(stderr, "zetta: unknown option '%s'\n", arg.c_str());
            o.bad_usage = true;
            break;
        }
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parse_args(argc, argv);
    if (opts.bad_usage) {
        print_usage(argv[0]);
        return 2;
    }
    if (opts.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    // One UUID is both the cluster node id and the replication actor id, so a
    // node's changes are attributed to it and merge conflict-free. Replication
    // is the source of truth for data; it must outlive the Node (which captures
    // into and reconciles from it) and the Cluster (whose thread gossips it).
    const std::string node_id = zetta::new_uuid();
    zetta::ReplicationManager replication(node_id);

    zetta::Node node(opts.node, &replication);
    if (!node.start()) {
        std::fprintf(stderr, "zetta: failed to start node\n");
        return 1;
    }

    // Large-object bodies transfer peer-to-peer (their metadata is in the CRDT).
    zetta::ReplicatedBlobStore blobs(opts.node.data_dir, replication);
    zetta::Cluster cluster(node_id, opts.cluster, &replication, &blobs);
    cluster.start();

    // Seed this node's membership entry now that its reachable address is known;
    // peers merge it so every node's document lists all nodes.
    replication.set("node:" + node_id,
                    nlohmann::json{{"address",
                                    cluster.listen_addresses().front()}});

    // Tell the operator where this node is reachable so they can connect more
    // nodes. A wildcard bind expands to every interface IPv4.
    std::printf("zetta: cluster node %s\n", cluster.node_id().c_str());
    for (const std::string& address : cluster.listen_addresses()) {
        std::printf("zetta: listening on %s\n", address.c_str());
    }
    std::printf("zetta: add another node with: %s\n",
                cluster.peer_command().c_str());

    int rc = 0;
    if (opts.mode == Mode::Headless) {
        rc = zetta::run_headless(node, cluster);
    } else {
        zetta::gui::App app(node, cluster);
        if (app.init()) {
            app.run();
        } else {
            rc = 1;
        }
    }

    cluster.stop();
    node.stop();
    return rc;
}
