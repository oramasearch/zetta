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
#include "zetta/cloud/node_bootstrap_env.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/cloud/provisioner_detect.hpp"
#include "zetta/cloud/provisioner_fleet.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/version.hpp"

#include "gui/app.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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
    bool require_auth = false;      // --require-auth / ZETTA_REQUIRE_AUTH (#745)
    std::string admin_token;        // --admin-token (bootstrap owner secret, #744)
    std::string join_token;         // --join-token (join an existing cluster)
    bool cluster_plaintext = false;  // --cluster-plaintext (debugging only)
    bool data_dir_from_cli = false;  // --data-dir set explicitly (vs default/env)
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
        "  --mcp-token T       Legacy shared-secret bearer (= acting key id);\n"
        "                      ignored when --require-auth is set\n"
        "  --require-auth      Default-deny: every HTTP/MCP/REST path needs a\n"
        "                      bearer that resolves to a user via the token\n"
        "                      store, else 401 (or ZETTA_REQUIRE_AUTH); #745\n"
        "  --admin-token T     Seed the owner user's access token from this\n"
        "                      secret on first boot (or ZETTA_ADMIN_TOKEN);\n"
        "                      hashed at rest, never logged (#744)\n"
        "  --no-mcp            Disable the embedded MCP server\n"
        "  --data-dir PATH     Database directory (default: %s)\n"
        "  --listen HOST:PORT  Cluster listen address (default: %s:%u);\n"
        "                      bound exactly or the node exits (no silent drift)\n"
        "  --port-range N      Bind the first free port in [PORT, PORT+N), to\n"
        "                      pack several nodes per host (default: %u = strict)\n"
        "  --peer HOST:PORT    A node to connect to (repeatable; all equal).\n"
        "                      HOST:PORT,HOST:PORT lists one peer's candidate\n"
        "                      endpoints — the reachable one is used\n"
        "  --advertise H:P[,..] Extra reachable endpoints to advertise to peers\n"
        "                      (a NAT'd / load-balanced node's public/LB address,\n"
        "                      beyond its bound listen address). Repeatable\n"
        "  --dial-out-only     This node has no routable address (behind NAT / a\n"
        "                      private VNet): advertise nothing dialable and only\n"
        "                      reach peers outbound; peers never dial it (#803)\n"
        "  --node-type T       permanent (default — durability-bearing, counts\n"
        "                      toward the replication factor) or ephemeral (burst\n"
        "                      capacity; locality replicas only, uncounted) (#809)\n"
        "  --join-token TOKEN  Cluster join token (printed by the first node);\n"
        "                      persisted in data-dir on first use\n"
        "  --cluster-plaintext Disable inter-node mTLS (debugging only —\n"
        "                      anyone who can reach the port can join)\n"
        "  --log-cluster       Log cluster gossip events to stderr (debugging)\n"
        "  --no-preflight      Skip the startup data integrity check\n"
        "  -h, --help          Show this help\n"
        "\n"
        "Environment (provisioning; applied as defaults, overridden by the\n"
        "flags above — a provisioned node auto-joins with no manual flags):\n"
        "  ZETTA_SEED_PEER     Seed peer(s) -> --peer (comma/space separated)\n"
        "  ZETTA_CLUSTER_TOKEN Cluster join token -> --join-token\n"
        "  ZETTA_DATA_DIR      Database directory -> --data-dir\n",
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
        } else if (arg == "--require-auth") {
            o.require_auth = true;
        } else if (arg == "--admin-token") {
            const char* v = take_value(argc, argv, i, o, "--admin-token");
            if (v) o.admin_token = v;
        } else if (arg == "--headless") {
            o.mode = Mode::Headless;
        } else if (arg == "--headed") {
            o.mode = Mode::Headed;
        } else if (arg == "--data-dir") {
            const char* v = take_value(argc, argv, i, o, "--data-dir");
            if (!v) break;
            o.node.data_dir = v;
            o.data_dir_from_cli = true;
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
        } else if (arg == "--advertise") {
            const char* v = take_value(argc, argv, i, o, "--advertise");
            if (!v) break;
            // A comma-separated set — split so each becomes one advertised
            // candidate endpoint (a NAT/LB node lists its public/LB address here).
            const std::string list = v;
            std::string::size_type start = 0;
            while (start <= list.size()) {
                const auto comma = list.find(',', start);
                const auto end = comma == std::string::npos ? list.size() : comma;
                std::string part = list.substr(start, end - start);
                while (!part.empty() && part.front() == ' ') part.erase(0, 1);
                while (!part.empty() && part.back() == ' ') part.pop_back();
                if (!part.empty()) o.cluster.advertise.push_back(std::move(part));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (arg == "--dial-out-only") {
            o.cluster.dial_out_only = true;
        } else if (arg == "--node-type") {
            const char* v = take_value(argc, argv, i, o, "--node-type");
            if (!v) break;
            o.cluster.node_type = v;
        } else if (arg == "--join-token") {
            const char* v = take_value(argc, argv, i, o, "--join-token");
            if (!v) break;
            o.join_token = v;
        } else if (arg == "--cluster-plaintext") {
            o.cluster_plaintext = true;
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
    Options opts = parse_args(argc, argv);
    if (opts.bad_usage) {
        print_usage(argv[0]);
        return 2;
    }
    if (opts.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    // Provisioning env (#749): a burst/provisioned node is handed its cluster
    // coordinates as environment by the provisioner seam (ZETTA_SEED_PEER,
    // ZETTA_CLUSTER_TOKEN, ZETTA_DATA_DIR; docs/plans/cloud-provisioning.md Gap
    // A). Fold them in as defaults beneath the flags already parsed — explicit
    // flag > env > default — so a provisioned node auto-joins a secure (mTLS)
    // cluster with no manual flags. A plaintext cluster stays plaintext (env
    // never forces security on); the --join-token/--cluster-plaintext conflict
    // check below still guards the result.
    {
        zetta::cloud::NodeBootstrap bootstrap;
        bootstrap.seeds = opts.cluster.seeds;
        bootstrap.join_token = opts.join_token;
        bootstrap.data_dir = opts.node.data_dir;
        bootstrap.cluster_plaintext = opts.cluster_plaintext;
        bootstrap.peer_from_cli = !opts.cluster.seeds.empty();
        bootstrap.join_token_from_cli = !opts.join_token.empty();
        bootstrap.data_dir_from_cli = opts.data_dir_from_cli;

        const zetta::cloud::BootstrapEnvApplied applied =
            zetta::cloud::apply_bootstrap_env(bootstrap);

        opts.cluster.seeds = std::move(bootstrap.seeds);
        opts.join_token = std::move(bootstrap.join_token);
        opts.node.data_dir = std::move(bootstrap.data_dir);

        // One startup line naming what env contributed — token redacted.
        if (applied.any()) {
            std::string summary = "zetta: applied provisioning env:";
            if (!applied.seeds.empty()) {
                summary += " ZETTA_SEED_PEER=";
                bool first = true;
                for (const std::string& seed : applied.seeds) {
                    if (!first) summary += ",";
                    summary += seed;
                    first = false;
                }
                summary += " (--peer)";
            }
            if (applied.join_token) {
                summary += " ZETTA_CLUSTER_TOKEN=**** (--join-token)";
            }
            if (!applied.data_dir.empty()) {
                summary += " ZETTA_DATA_DIR=" + applied.data_dir + " (--data-dir)";
            }
            std::printf("%s\n", summary.c_str());
        }
    }

    if (opts.cluster_plaintext && !opts.join_token.empty()) {
        std::fprintf(stderr,
                     "zetta: --cluster-plaintext and --join-token conflict\n");
        return 2;
    }

    // One UUID is both the cluster node id and the replication actor id, so a
    // node's changes are attributed to it and merge conflict-free. It's stable
    // across restarts (persisted in data_dir/node_id) so the change history
    // doesn't fragment and membership keys don't accumulate. Replication is the
    // source of truth for data; it must outlive the Node (which captures into and
    // reconciles from it) and the Cluster (whose thread gossips it).
    const std::string node_id = load_or_create_node_id(opts.node.data_dir);

    // Inter-node transport security (CTL-2, #466): secure by default. The
    // first start generates a join token into data_dir/security/ (or adopts
    // --join-token) and mints this node's identity; the Cluster then refuses
    // any connection that can't prove that token's CA. Plaintext is an
    // explicit opt-out, for debugging only.
    if (!opts.cluster_plaintext) {
        std::string reason;
        std::optional<zetta::cluster::ClusterSecurity> security =
            zetta::cluster::load_or_create_cluster_security(
                opts.node.data_dir, node_id, opts.join_token, &reason);
        if (!security) {
            std::fprintf(stderr, "zetta: cluster security setup failed: %s\n",
                         reason.c_str());
            return 1;
        }
        opts.cluster.security = std::move(*security);
    } else {
        std::fprintf(stderr,
                     "zetta: WARNING inter-node TLS disabled "
                     "(--cluster-plaintext): any host that can reach the "
                     "listen port can join this cluster\n");
    }

    zetta::cluster::ReplicationManager replication(node_id);

    zetta::Node node(opts.node, &replication);

    // Crash reports (#640, docs/architecture/observability.md): from here on a
    // fatal signal writes <data_dir>/crashes/<uuid>.json — stacktrace, recent
    // spans, version/platform — before the process dies. Reports from previous
    // runs are surfaced now (and queryable as system.crashes) — in BOTH modes.
    zetta::observability::CrashHandlerOptions crash_options;
    crash_options.crashes_dir =
        (std::filesystem::path(opts.node.data_dir) / "crashes").string();
    crash_options.recorder = &node.span_recorder();
    std::string crash_error;
    if (!zetta::observability::install_crash_handler(crash_options,
                                                     &crash_error)) {
        std::fprintf(stderr, "zetta: crash reports disabled: %s\n",
                     crash_error.c_str());
    }
    const auto previous_crashes =
        zetta::observability::load_crash_reports(crash_options.crashes_dir);
    if (!previous_crashes.empty()) {
        std::printf("zetta: %zu crash report(s) from previous runs — "
                    "SELECT * FROM system.crashes;\n",
                    previous_crashes.size());
    }

    if (!node.start()) {
        std::fprintf(stderr, "zetta: failed to start node\n");
        return 1;
    }

    // Bootstrap the owner (#733 / #744): on an empty user store, mint an "owner"
    // user + admin role + an access token seeded from --admin-token (or the
    // ZETTA_ADMIN_TOKEN env). Closes the seeding gap so a node is never locked
    // out. We log WHERE the token lives, never its value — the operator already
    // holds the secret they supplied.
    {
        std::string admin_secret = opts.admin_token;
        if (admin_secret.empty()) {
            if (const char* env = std::getenv("ZETTA_ADMIN_TOKEN")) {
                admin_secret = env;
            }
        }
        if (!admin_secret.empty()) {
            const zetta::Node::BootstrapResult boot =
                node.bootstrap_owner(admin_secret);
            if (boot.performed) {
                std::printf(
                    "zetta: bootstrapped owner user '%s' + admin role + access "
                    "token %s (stored hashed under %s) — authenticate with the "
                    "admin token you supplied\n",
                    boot.user_name.c_str(), boot.token_id.c_str(),
                    (std::filesystem::path(opts.node.data_dir) / boot.token_id)
                        .c_str());
            } else {
                std::printf("zetta: owner bootstrap skipped (%s)\n",
                            boot.reason.c_str());
            }
        } else if (node.user_count() == 0) {
            std::printf(
                "zetta: no users yet — seed an owner with --admin-token "
                "<secret> or ZETTA_ADMIN_TOKEN (the secret is hashed at rest, "
                "never logged; #744)\n");
        }
    }

    // Large-object bodies transfer peer-to-peer (their metadata is in the CRDT).
    zetta::data::ReplicatedBlobStore blobs(opts.node.data_dir, replication);
    zetta::cluster::Cluster cluster(node_id, opts.cluster, &replication, &blobs);
    cluster.start();

    // Honor --listen strictly (#738): a node's peer address is load-bearing —
    // siblings dial it and it's gossiped as membership — so if the requested
    // port couldn't be bound we exit non-zero rather than silently drift to
    // another port and fall off the mesh. SO_REUSEADDR already lets us rebind
    // across a TIME_WAIT window; this catches the case where the port is still
    // actively held (e.g. a k8s rolling restart overlapping the old process),
    // so a supervisor restarts us cleanly once it frees. Widen --port-range to
    // opt into binding the first free port in a range instead.
    if (!cluster.is_listening()) {
        std::fprintf(stderr,
                     "zetta: could not bind cluster listener on %s:%u — port in "
                     "use. Refusing to start rather than drift to another port "
                     "(#738). Free the port, or pass --port-range N to bind the "
                     "first free port in a range.\n",
                     opts.cluster.listen_host.c_str(),
                     static_cast<unsigned>(opts.cluster.listen_port));
        cluster.stop();
        node.stop();
        return 1;
    }

    // Seed this node's membership entry now that its reachable address is known;
    // peers merge it so every node's document lists all nodes. The durable tier of
    // system.nodes (#809): `address` is the node's DIALABLE address (the --advertise
    // front for a wan node behind an LB, else its bound address) and empty for a
    // dial-out-only node; `reachability` tells transitive learners whether they may
    // dial it (#803); `node_type` (permanent|ephemeral) is the durability class (RF
    // is maintained over permanent nodes). Ephemeral / liveness / telemetry columns
    // are overlaid at query time and never written here.
    replication.set(
        "node:" + node_id,
        nlohmann::json{{"address", cluster.primary_advertised_address()},
                       {"reachability", cluster.dial_out_only()
                                            ? "dial-out-only"
                                            : "wan-accessible"},
                       {"node_type", cluster.node_type()}});

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
    if (opts.cluster.security.enabled()) {
        std::printf("zetta: inter-node mTLS on (join token in %s)\n",
                    (std::filesystem::path(opts.node.data_dir) / "security" /
                     "cluster-secret.json")
                        .c_str());
    }
    std::printf("zetta: add another node with: %s\n",
                cluster.peer_command().c_str());

    // Detect which cloud-burst targets this node can drive from the operator's own
    // local CLIs (`az` logged in, …) and record each on the node so it surfaces as
    // system.provisioners — in BOTH modes, since the table is useful headless
    // too. No credentials are stored: the CLI keeps its own auth, and the control
    // plane uses that same CLI to act. The headed App then wires a Provisioner per
    // detected target into its consent-gated Add-nodes control (below).
    for (zetta::cloud::ProvisionerConfig& detected : zetta::cloud::detect_provisioner_configs()) {
        std::printf("zetta: detected cloud provisioner: %s — %s\n",
                    detected.target.c_str(), detected.summary.c_str());
        node.record_provisioner_config(std::move(detected));
    }

    // The provisioners the "Add nodes" controls drive — the headed Cluster
    // view AND the MCP surface's consent-gated add_nodes capability (#621), in
    // BOTH modes. Declared before the ApiServer / App so they outlive every
    // consumer holding references into them.
    //
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

    // Cloud-burst targets: a CLI-backed Provisioner per context detected
    // above. A target with no detected context stays unwired (the consent
    // gates then read "no provisioner wired"), unchanged.
    zetta::cloud::ProvisionerFleet provisioner_fleet;
    const std::string default_seed_peer = cluster.listen_addresses().empty()
                                              ? std::string{}
                                              : cluster.listen_addresses().front();
    // On a secure (mTLS) cluster, hand each burst node this cluster's join token
    // too — so a provisioned node boots with `--peer <seed> --join-token <token>`
    // and can join a non-plaintext cluster instead of being stuck on
    // --cluster-plaintext (#670). Empty under --cluster-plaintext, so nothing is
    // injected. The token is a secret: it rides the same bootstrap env seam as the
    // seed peer (ZETTA_CLUSTER_TOKEN alongside ZETTA_SEED_PEER) and is never stored
    // in the detected provisioner config / system.provisioners.
    const std::string default_cluster_token = opts.cluster.security.join_token;
    for (const zetta::cloud::ProvisionerFleet::Wiring& wiring :
         provisioner_fleet.wire(node.provisioner_configs(), default_seed_peer,
                                default_cluster_token)) {
        if (wiring.wired) {
            std::printf("zetta: cloud provisioner wired for %s (local CLI)\n",
                        wiring.id.c_str());
        }
    }
    const auto provisioner_for =
        [&local_provisioner,
         &provisioner_fleet](const std::string& target_id) -> zetta::cloud::Provisioner* {
        if (target_id == "local") return &local_provisioner;
        return provisioner_fleet.provisioner_for(target_id);
    };

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
        // The default-deny gate (#745): --require-auth or ZETTA_REQUIRE_AUTH
        // closes every path unless the bearer resolves to a user via the token
        // store. Secure-by-default when set; off keeps the loopback GUI's
        // unauthenticated-local session working (back-compat).
        bool require_auth = opts.require_auth;
        if (!require_auth) {
            if (const char* env = std::getenv("ZETTA_REQUIRE_AUTH")) {
                const std::string value = env;
                require_auth =
                    !(value.empty() || value == "0" || value == "false");
            }
        }
        api_options.require_auth = require_auth;
        api_server = std::make_unique<zetta::api::ApiServer>(node, &cluster,
                                                             api_options);
        if (api_server->start()) {
            const char* auth_note =
                require_auth ? " (auth required — token resolves to a user)"
                : opts.mcp_token.empty() ? " (unauthenticated local)"
                                         : " (bearer token required)";
            std::printf("zetta: MCP endpoint http://%s:%u/mcp%s\n",
                        api_options.listen_address.c_str(),
                        static_cast<unsigned>(api_server->port()), auth_note);
            if (require_auth && node.token_count() == 0) {
                std::fprintf(stderr,
                             "zetta: WARNING --require-auth is on but no tokens "
                             "exist — every request will 401. Seed an owner with "
                             "--admin-token <secret> (#745/#733).\n");
            }
        } else {
            std::fprintf(stderr, "zetta: MCP server failed to start: %s\n",
                         api_server->reason().c_str());
            api_server.reset();
        }
    }
    if (api_server) {
        // The consent-gated add_nodes capability drives the same wired
        // provisioners the headed control does (#621). The fleet + local
        // provisioner outlive the server (declared above).
        api_server->set_provisioner_resolver(provisioner_for);
    }
    const auto drain_api = [&api_server] {
        if (api_server) api_server->drain();
    };

    int rc = 0;
    if (opts.mode == Mode::Headless) {
        rc = zetta::run_headless(node, cluster, drain_api);
    } else {
        zetta::gui::App app(node, cluster);
        app.set_tick_hook(drain_api);
        // Live surfacing of externally-initiated actions (#620): every
        // successful mutating capability call (an MCP agent, a future REST
        // integration) lands in the headed app, which surfaces it above every
        // view with a click-through into the owning panel. The hook fires on
        // this (owning) loop inside drain_api; App buffers it thread-safely
        // regardless. Cleared below before the App is destroyed.
        if (api_server) {
            api_server->set_activity_hook(
                [&app](const zetta::api::ActivityEvent& event) {
                    app.notify_external_activity(event.capability, event.actor);
                });
        }
        // GUI-driven searches record their trace into the same span ring MCP
        // calls use, so list_traces shows one history (no ring = no spans).
        if (api_server) app.set_span_recorder(&api_server->recorder());
        // The command palette (#625) renders + executes the same capability
        // registry the MCP server serves — one list, many renderings. With
        // --no-mcp the App builds its own from register_core_capabilities.
        if (api_server) app.set_capability_registry(&api_server->registry());

        // The Cluster view's "Add nodes" control drives the same fleet the
        // MCP surface resolves (declared above, so it outlives the App).
        app.set_provisioner("local", local_provisioner);
        for (const std::string& target :
             {std::string{"azure"}, std::string{"aws"}, std::string{"gcp"},
              std::string{"oci"}, std::string{"kubernetes"},
              std::string{"bare-metal"}}) {
            if (zetta::cloud::Provisioner* wired =
                    provisioner_fleet.provisioner_for(target)) {
                app.set_provisioner(target, *wired);
            }
        }

        if (app.init()) {
            app.run();
        } else {
            rc = 1;
        }
        // The App is about to be destroyed while the API server lives on a
        // little longer — drop the hook so nothing can reach the dead App
        // (no further drain runs, so no call should fire it; this is a belt).
        if (api_server) api_server->set_activity_hook({});
    }

    cluster.stop();
    node.stop();
    return rc;
}
