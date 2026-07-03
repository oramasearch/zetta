// zetta — entry point.
//
// Parses arguments, builds a zetta::Node + a zetta::cluster::Cluster, and dispatches to
// one of two run modes. Headed (the default) renders the node in an SDL3 + Dear
// ImGui window; headless runs the same node with no UI, for servers with no
// display. All the real work lives behind zetta::Node / zetta::cluster::Cluster — main
// only picks how to drive them.

#include "zetta/cluster/cluster.hpp"
#include "zetta/api/server.hpp"
#include "zetta/headless.hpp"
#include "zetta/cloud/local_provisioner.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/cloud/provisioner_detect.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/version.hpp"

#include "gui/app.hpp"
#include "gui/cloud_wiring.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

enum class Mode { Headed, Headless };

struct Options {
    Mode mode = Mode::Headed;  // headed by default
    zetta::NodeConfig node;
    zetta::cluster::ClusterConfig cluster;
    bool mcp_enabled = true;                    // --no-mcp disables
    std::string mcp_listen = "127.0.0.1:4800";  // --mcp-listen HOST:PORT
    std::string mcp_token;                      // --mcp-token (acting key id)
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
    const zetta::cluster::ClusterConfig defaults;
    std::printf(
        "zetta %s — a database node\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --headed            Run with the SDL3 + ImGui UI (default)\n"
        "  --headless          Run without a UI (server only)\n"
        "  --mcp-listen H:P    MCP endpoint bind (default 127.0.0.1:4800)\n"
        "  --mcp-token T       Require this bearer token (= acting key id)\n"
        "  --no-mcp            Disable the embedded MCP server\n"
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
        if (arg == "--no-mcp") {
            o.mcp_enabled = false;
        } else if (arg == "--mcp-listen") {
            const char* v = take_value(argc, argv, i, o, "--mcp-listen");
            if (v) o.mcp_listen = v;
        } else if (arg == "--mcp-token") {
            const char* v = take_value(argc, argv, i, o, "--mcp-token");
            if (v) o.mcp_token = v;
        } else if (arg == "--headless") {
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

// A node's identity (its cluster node id and replication actor id) must be STABLE
// across restarts: a fresh id each run would fragment the Automerge change
// history across many actors and leave a stale `node:` membership key behind
// every time. So persist it in `data_dir/node_id` and reuse it.
std::string load_or_create_node_id(const std::string& data_dir) {
    namespace fs = std::filesystem;
    const fs::path path = fs::path(data_dir) / "node_id";
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    if (std::ifstream in(path); in) {
        std::string id;
        std::getline(in, id);
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r' ||
                               id.back() == ' ')) {
            id.pop_back();
        }
        if (!id.empty()) return id;
    }
    const std::string id = zetta::util::new_uuid();
    std::ofstream(path, std::ios::trunc) << id << '\n';
    return id;
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
    // node's changes are attributed to it and merge conflict-free. It's stable
    // across restarts (persisted in data_dir/node_id) so the change history
    // doesn't fragment and membership keys don't accumulate. Replication is the
    // source of truth for data; it must outlive the Node (which captures into and
    // reconciles from it) and the Cluster (whose thread gossips it).
    const std::string node_id = load_or_create_node_id(opts.node.data_dir);
    zetta::cluster::ReplicationManager replication(node_id);

    zetta::Node node(opts.node, &replication);
    if (!node.start()) {
        std::fprintf(stderr, "zetta: failed to start node\n");
        return 1;
    }

    // Large-object bodies transfer peer-to-peer (their metadata is in the CRDT).
    zetta::data::ReplicatedBlobStore blobs(opts.node.data_dir, replication);
    zetta::cluster::Cluster cluster(node_id, opts.cluster, &replication, &blobs);
    cluster.start();

    // Seed this node's membership entry now that its reachable address is known;
    // peers merge it so every node's document lists all nodes.
    replication.set("node:" + node_id,
                    nlohmann::json{{"address",
                                    cluster.listen_addresses().front()}});

    // Give the running node its long-lived compute plane: a JobCoordinator over
    // the real cluster's signal plane (#579), so distributed jobs submitted
    // through node.job_coordinator() run SPMD across live membership. Attached
    // here (not at construction) because the Node is built + started before the
    // Cluster exists; node.stop() tears it down before the cluster shuts down.
    node.attach_cluster(cluster);

    // Tell the operator where this node is reachable so they can connect more
    // nodes. A wildcard bind expands to every interface IPv4.
    std::printf("zetta: cluster node %s\n", cluster.node_id().c_str());
    for (const std::string& address : cluster.listen_addresses()) {
        std::printf("zetta: listening on %s\n", address.c_str());
    }
    std::printf("zetta: add another node with: %s\n",
                cluster.peer_command().c_str());

    // Detect which cloud-burst targets this node can drive from the operator's own
    // local CLIs (`az` logged in, …) and record each on the node so it surfaces as
    // system.zetta_provisioners — in BOTH modes, since the table is useful headless
    // too. No credentials are stored: the CLI keeps its own auth, and the control
    // plane uses that same CLI to act. The headed App then wires a Provisioner per
    // detected target into its consent-gated Add-nodes control (below).
    for (zetta::cloud::ProvisionerConfig& detected : zetta::cloud::detect_provisioner_configs()) {
        std::printf("zetta: detected cloud provisioner: %s — %s\n",
                    detected.target.c_str(), detected.summary.c_str());
        node.record_provisioner_config(std::move(detected));
    }

    // The embedded MCP server (docs/api/registry-design.md, #615): the
    // programmatic control surface, in BOTH modes. Handlers marshal onto this
    // (owning) loop via the bridge the tick hooks drain below.
    std::unique_ptr<zetta::api::ApiServer> api_server;
    if (opts.mcp_enabled) {
        zetta::api::ServerOptions api_options;
        const auto colon = opts.mcp_listen.rfind(':');
        std::uint16_t api_port = 0;
        if (colon == std::string::npos ||
            !parse_uint16(opts.mcp_listen.substr(colon + 1), api_port)) {
            std::fprintf(stderr,
                         "zetta: bad --mcp-listen '%s' (want HOST:PORT)\n",
                         opts.mcp_listen.c_str());
            return 2;
        }
        api_options.listen_address = opts.mcp_listen.substr(0, colon);
        api_options.port = api_port;
        api_options.bearer_token = opts.mcp_token;
        api_server = std::make_unique<zetta::api::ApiServer>(node, &cluster,
                                                             api_options);
        if (api_server->start()) {
            std::printf("zetta: MCP endpoint http://%s:%u/mcp%s\n",
                        api_options.listen_address.c_str(),
                        static_cast<unsigned>(api_server->port()),
                        opts.mcp_token.empty() ? " (unauthenticated local)"
                                               : " (bearer token required)");
        } else {
            std::fprintf(stderr, "zetta: MCP server failed to start: %s\n",
                         api_server->reason().c_str());
            api_server.reset();
        }
    }
    const auto drain_api = [&api_server] {
        if (api_server) api_server->drain();
    };

    int rc = 0;
    if (opts.mode == Mode::Headless) {
        rc = zetta::run_headless(node, cluster, drain_api);
    } else {
        // The provisioners the Cluster view's "Add nodes" control drives. Both
        // holders are declared BEFORE the App so they outlive it: App keeps raw
        // references to them (set_provisioner), so it must be destroyed first.

        // Given-hardware target: a LocalProvisioner that reports the nodes already
        // in the cluster (best-effort — it brings up no new machines and touches no
        // cloud), reading the live fleet from Cluster::nodes() on each call so it
        // tracks self-healing membership.
        zetta::cloud::LocalProvisioner local_provisioner([&cluster] {
            std::vector<zetta::cloud::ProvisionedNode> fleet;
            for (const zetta::cluster::NodeStatus& peer : cluster.nodes()) {
                fleet.push_back({peer.node_id, peer.address, {}});
            }
            return fleet;
        });

        // Cloud-burst targets: a CLI-backed Provisioner per context detected above.
        // A target with no detected context stays unwired (the Add-nodes consent
        // gate then shows "no provisioner wired"), unchanged.
        zetta::gui::CloudProvisioners cloud_provisioners;

        zetta::gui::App app(node, cluster);
        app.set_tick_hook(drain_api);
        app.set_provisioner("local", local_provisioner);

        // Burst nodes auto-join this cluster by seeding from a reachable address.
        const std::string seed_peer = cluster.listen_addresses().empty()
                                          ? std::string{}
                                          : cluster.listen_addresses().front();
        for (const zetta::gui::CloudProvisioners::Wiring& wiring :
             cloud_provisioners.wire(app, node.provisioner_configs(), seed_peer)) {
            if (wiring.wired) {
                std::printf(
                    "zetta: cloud provisioner wired for %s (local CLI)\n",
                    wiring.id.c_str());
            }
        }

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
