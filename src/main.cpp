// zetta — entry point.
//
// Parses arguments, builds a zetta::Node + a zetta::cluster::Cluster, and dispatches to
// one of two run modes. Headed (the default) renders the node in an SDL3 + Dear
// ImGui window; headless runs the same node with no UI, for servers with no
// display. All the real work lives behind zetta::Node / zetta::cluster::Cluster — main
// only picks how to drive them.

#include "zetta/api/clerk_auth.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/api/native_http_transport.hpp"
#include "zetta/api/mysql_protocol.hpp"
#include "zetta/api/postgres_protocol.hpp"
#include "zetta/api/server.hpp"
#include "zetta/compute/mlx_serving.hpp"
#include "zetta/connect/egress_policy.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/connect/connector_destinations.hpp"
#include "zetta/connect/model_fetch.hpp"
#include "zetta/headless.hpp"
#include "zetta/cloud/local_provisioner.hpp"
#include "zetta/cloud/node_bootstrap_env.hpp"
#include "zetta/governance/identity_seal.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/cloud/provisioner_detect.hpp"
#include "zetta/cloud/provisioner_fleet.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/token_secret.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/version.hpp"

#include "gui/app.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

enum class Mode { Headed, Headless };

class NativeHttpTransportLifetime {
public:
    NativeHttpTransportLifetime() {
        zetta::api::install_native_http_transport();
    }

    ~NativeHttpTransportLifetime() {
        zetta::api::shutdown_native_http_transport();
    }

    NativeHttpTransportLifetime(const NativeHttpTransportLifetime&) = delete;
    NativeHttpTransportLifetime& operator=(const NativeHttpTransportLifetime&) =
        delete;
};

// Records every connection the egress policy refuses (CTL-4, #75) on the node's
// audit trail, and stops before the Node goes away.
class EgressDenialAudit {
public:
    explicit EgressDenialAudit(zetta::Node& node) {
        zetta::connect::set_egress_denial_observer(
            [&node](const zetta::connect::EgressDenial& denial) {
                zetta::governance::AuditEventValue event;
                event.action = zetta::governance::AuditAction::EgressDenied;
                event.actor = "node";
                event.target = "egress:" + denial.host + ":" +
                               std::to_string(denial.port);
                event.allowed = false;
                event.detail = denial.reason;
                node.record_audit_event(std::move(event));
            });
    }

    ~EgressDenialAudit() { zetta::connect::set_egress_denial_observer({}); }

    EgressDenialAudit(const EgressDenialAudit&) = delete;
    EgressDenialAudit& operator=(const EgressDenialAudit&) = delete;
};

// The `host[:port]` of an http(s) URL as an egress allowlist entry, or empty.
std::string url_authority(std::string_view url) {
    std::string_view rest;
    std::string_view port;
    if (url.starts_with("https://")) {
        rest = url.substr(8);
        port = "443";
    } else if (url.starts_with("http://")) {
        rest = url.substr(7);
        port = "80";
    } else {
        return {};
    }
    const auto end = rest.find_first_of("/?#");
    std::string authority(rest.substr(0, end));
    if (authority.empty() || authority.find('@') != std::string::npos) return {};
    const bool has_port = authority.starts_with("[")
                              ? authority.find("]:") != std::string::npos
                              : authority.find(':') != std::string::npos;
    if (!has_port) authority += ":" + std::string(port);
    return authority;
}

// The node's egress allowlist: --egress-allow, else ZETTA_EGRESS_ALLOW, plus the
// Clerk issuer when one is configured (the auth gate fetches its JWKS). Empty
// means deny: only loopback is reachable. Returns nullopt on a malformed entry.
std::optional<zetta::connect::EgressPolicy> configured_egress_policy(
    const std::vector<std::string>& from_cli, const std::string& clerk_issuer) {
    std::string list;
    for (const std::string& entry : from_cli) list += entry + ",";
    if (from_cli.empty()) {
        if (const char* env = std::getenv("ZETTA_EGRESS_ALLOW")) list = env;
    }
    std::string error;
    auto policy = zetta::connect::EgressPolicy::parse(list, &error);
    if (!policy) {
        std::fprintf(stderr, "zetta: %s\n", error.c_str());
        return std::nullopt;
    }
    if (const std::string issuer = url_authority(clerk_issuer); !issuer.empty()) {
        if (!policy->add(issuer, &error)) {
            std::fprintf(stderr, "zetta: the Clerk issuer: %s\n", error.c_str());
            return std::nullopt;
        }
    }
    return policy;
}

struct Options {
    Mode mode = Mode::Headed;  // headed by default
    zetta::NodeConfig node;
    zetta::cluster::ClusterConfig cluster;
    bool mcp_enabled = true;                    // --no-mcp disables
    std::string mcp_listen = "127.0.0.1:4800";  // --mcp-listen HOST:PORT
    std::string webapp_dir;                     // --webapp-dir PATH (the Studio SPA bundle)
    std::string clerk_issuer;                   // --clerk-issuer URL (Clerk JWT verify; else bearer-only)
    std::optional<std::string> connector_allow_destinations;  // exact host:port entries (#1414)
    std::optional<std::string> action_allow_destinations;  // --action-allow-destinations LIST (#1394)
    std::string admin_token;        // --admin-token (bootstrap owner secret, #744)
    std::string join_token;         // --join-token (join an existing cluster)
    std::vector<std::string> egress_allow;  // --egress-allow (repeatable; CTL-4, #75)
    std::string identity_root;      // --identity-root (the cluster's identity root, #764)
    std::string identity_min_epoch; // --identity-min-epoch (#1360)
    bool data_dir_from_cli = false;  // --data-dir set explicitly (vs default/env)
    bool pubsub_dispatch_mode_from_cli = false;
    bool pubsub_topics_per_thread_from_cli = false;
    bool pubsub_dispatch_min_threads_from_cli = false;
    bool pubsub_dispatch_max_threads_from_cli = false;
    bool audit_retention_from_cli = false;
    bool audit_summary_retention_from_cli = false;
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

bool parse_positive_size(std::string_view text, std::size_t& out) {
    std::size_t value = 0;
    const auto [pointer, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || pointer != text.data() + text.size() || value == 0) {
        return false;
    }
    out = value;
    return true;
}

bool parse_uint32(const std::string& text, std::uint32_t& out) {
    const auto [pointer, error_code] =
        std::from_chars(text.data(), text.data() + text.size(), out);
    return error_code == std::errc{} &&
           pointer == text.data() + text.size();
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
        "  --webapp-dir PATH   Serve the built Studio web app at /app from PATH\n"
        "  --admin-token T     Seed the owner user's access token from this\n"
        "                      secret on first boot (or ZETTA_ADMIN_TOKEN);\n"
        "                      hashed at rest, never logged. If omitted on a\n"
        "                      fresh node, one is generated and printed once so\n"
        "                      the node is never locked out (#744/#733)\n"
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
        "                      persisted in data-dir on first use. Inter-node\n"
        "                      mTLS is always on — there is no plaintext mode\n"
        "  --identity-root KEY The cluster's identity root public key (printed\n"
        "                      by the first node; or ZETTA_IDENTITY_ROOT). Pinned\n"
        "                      in data-dir on first start: the node honors only\n"
        "                      accounts, roles and tokens sealed by this root.\n"
        "                      Not needed when the node gets the cluster's\n"
        "                      --admin-token, which derives the same root (#764)\n"
        "  --identity-min-epoch N  Honor no account, role or token until an identity\n"
        "                      epoch at least N arrives (or ZETTA_IDENTITY_MIN_EPOCH).\n"
        "                      Read N from get_identity_epoch on a live node (#1360)\n"
        "  --log-cluster       Log cluster gossip events to stderr (debugging)\n"
        "  --no-preflight      Skip the startup data integrity check\n"
        "  --pubsub-dispatch-mode MODE\n"
        "                      pool (default), auto, per_key, or inline\n"
        "  --pubsub-topics-per-thread N\n"
        "                      Active keys per pool worker (default 250)\n"
        "  --pubsub-dispatch-threads N\n"
        "                      Minimum shared-pool workers (default 1)\n"
        "  --pubsub-dispatch-max-threads N\n"
        "                      Maximum workers (default detected CPU count)\n"
        "  --audit-retention-days N\n"
        "                      Keep ordinary audit rows for N days (default 30);\n"
        "                      zero keeps them forever\n"
        "  --audit-summary-retention-days N\n"
        "                      Keep audit summaries for N days (default 365);\n"
        "                      zero keeps them forever\n"
        "  --governance        Enforce consent gating on cloud-touching calls.\n"
        "                      OFF by default (#995): a consent-gated capability\n"
        "                      executes on the first call and the bypass is\n"
        "                      audited as consent_bypassed. Turn this on to\n"
        "                      require a distinctly-privileged approval again\n"
        "  --connector-allow-destinations LIST\n"
        "                exact host:port entries allowed for private/loopback connectors\n"
        "  --action-allow-destinations LIST\n"
        "                      Address classes an action or report delivery\n"
        "                      endpoint may reach besides public addresses:\n"
        "                      loopback, private, link-local, metadata\n"
        "                      (comma separated; default none) (#1394)\n"
        "  --egress-allow LIST Outside destinations this node may connect to\n"
        "                      (repeatable; comma separated): host, *.domain,\n"
        "                      address or CIDR range, each optionally :port;\n"
        "                      * allows everything. Outbound is DENIED by\n"
        "                      default; loopback is always allowed (CTL-4,\n"
        "                      docs/governance/egress-policy.md)\n"
        "  -h, --help          Show this help\n"
        "\n"
        "Environment (provisioning; applied as defaults, overridden by the\n"
        "flags above — a provisioned node auto-joins with no manual flags):\n"
        "  ZETTA_SEED_PEER     Seed peer(s) -> --peer (comma/space separated)\n"
        "  ZETTA_CLUSTER_TOKEN Cluster join token -> --join-token\n"
        "  ZETTA_CLUSTER_TOKEN_FILE File holding the join token (preferred)\n"
        "  ZETTA_DATA_DIR      Database directory -> --data-dir\n"
        "  ZETTA_WEBAPP_DIR    Studio web app bundle -> --webapp-dir\n"
        "  ZETTA_PUBSUB_DISPATCH_MODE -> --pubsub-dispatch-mode\n"
        "  ZETTA_PUBSUB_TOPICS_PER_THREAD -> --pubsub-topics-per-thread\n"
        "  ZETTA_PUBSUB_DISPATCH_THREADS -> --pubsub-dispatch-threads\n"
        "  ZETTA_PUBSUB_DISPATCH_MAX_THREADS -> --pubsub-dispatch-max-threads\n"
        "  ZETTA_GOVERNANCE    1/true/yes/on -> --governance\n"
        "  ZETTA_CONNECTOR_ALLOW_DESTINATIONS -> --connector-allow-destinations\n"
        "  ZETTA_ACTION_ALLOW_DESTINATIONS -> --action-allow-destinations\n"
        "  ZETTA_EGRESS_ALLOW  Egress allowlist -> --egress-allow\n"
        "  ZETTA_AUDIT_RETENTION_DAYS\n"
        "                      Ordinary audit retention in days\n"
        "  ZETTA_AUDIT_SUMMARY_RETENTION_DAYS\n"
        "                      Audit summary retention in days\n"
        "  ZETTA_MALLOC_TRIM_THRESHOLD_BYTES\n"
        "                      Opt-in free-held threshold for malloc_trim(0)\n",
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
        } else if (arg == "--webapp-dir") {
            const char* v = take_value(argc, argv, i, o, "--webapp-dir");
            if (v) o.webapp_dir = v;
        } else if (arg == "--clerk-issuer") {
            const char* v = take_value(argc, argv, i, o, "--clerk-issuer");
            if (v) o.clerk_issuer = v;
        } else if (arg == "--mcp-listen") {
            const char* v = take_value(argc, argv, i, o, "--mcp-listen");
            if (v) o.mcp_listen = v;
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
        } else if (arg == "--identity-root") {
            const char* v = take_value(argc, argv, i, o, "--identity-root");
            if (!v) break;
            o.identity_root = v;
        } else if (arg == "--identity-min-epoch") {
            const char* v = take_value(argc, argv, i, o, "--identity-min-epoch");
            if (!v) break;
            o.identity_min_epoch = v;
        } else if (arg == "--log-cluster") {
            o.cluster.log_events = true;
        } else if (arg == "--no-preflight") {
            o.node.preflight = false;
        } else if (arg == "--pubsub-dispatch-mode") {
            const char* value = take_value(argc, argv, i, o, "--pubsub-dispatch-mode");
            if (!value) break;
            const auto mode = zetta::pubsub::parse_dispatch_mode(value);
            if (!mode) {
                std::fprintf(stderr,
                             "zetta: --pubsub-dispatch-mode wants pool, auto, per_key, or inline\n");
                o.bad_usage = true;
                break;
            }
            o.node.pubsub_dispatch_mode = *mode;
            o.pubsub_dispatch_mode_from_cli = true;
        } else if (arg == "--pubsub-topics-per-thread") {
            const char* value = take_value(argc, argv, i, o, "--pubsub-topics-per-thread");
            if (!value) break;
            if (!parse_positive_size(value, o.node.pubsub_topics_per_thread)) {
                std::fprintf(stderr, "zetta: --pubsub-topics-per-thread wants a positive number\n");
                o.bad_usage = true;
                break;
            }
            o.pubsub_topics_per_thread_from_cli = true;
        } else if (arg == "--pubsub-dispatch-threads") {
            const char* value = take_value(argc, argv, i, o, "--pubsub-dispatch-threads");
            if (!value) break;
            if (!parse_positive_size(value, o.node.pubsub_dispatch_min_threads)) {
                std::fprintf(stderr, "zetta: --pubsub-dispatch-threads wants a positive number\n");
                o.bad_usage = true;
                break;
            }
            o.pubsub_dispatch_min_threads_from_cli = true;
        } else if (arg == "--pubsub-dispatch-max-threads") {
            const char* value = take_value(argc, argv, i, o, "--pubsub-dispatch-max-threads");
            if (!value) break;
            std::size_t maximum = 0;
            if (!parse_positive_size(value, maximum)) {
                std::fprintf(stderr, "zetta: --pubsub-dispatch-max-threads wants a positive number\n");
                o.bad_usage = true;
                break;
            }
            o.node.pubsub_dispatch_max_threads = maximum;
            o.pubsub_dispatch_max_threads_from_cli = true;
        } else if (arg == "--audit-retention-days") {
            const char* value =
                take_value(argc, argv, i, o, "--audit-retention-days");
            if (!value) {
                break;
            }
            std::uint32_t days = 0;
            if (!parse_uint32(value, days)) {
                std::fprintf(stderr,
                             "zetta: --audit-retention-days wants a non-negative number\n");
                o.bad_usage = true;
                break;
            }
            o.node.audit_retention = std::chrono::hours(24ULL * days);
            o.audit_retention_from_cli = true;
        } else if (arg == "--audit-summary-retention-days") {
            const char* value = take_value(
                argc, argv, i, o, "--audit-summary-retention-days");
            if (!value) {
                break;
            }
            std::uint32_t days = 0;
            if (!parse_uint32(value, days)) {
                std::fprintf(
                    stderr,
                    "zetta: --audit-summary-retention-days wants a non-negative number\n");
                o.bad_usage = true;
                break;
            }
            o.node.audit_summary_retention = std::chrono::hours(24ULL * days);
            o.audit_summary_retention_from_cli = true;
        } else if (arg == "--governance") {
            o.node.enforce_consent = true;
        } else if (arg == "--connector-allow-destinations") {
            const char* value = take_value(argc, argv, i, o, "--connector-allow-destinations");
            if (value) o.connector_allow_destinations = value;
        } else if (arg == "--action-allow-destinations") {
            const char* v = take_value(argc, argv, i, o, "--action-allow-destinations");
            if (v) o.action_allow_destinations = v;
        } else if (arg == "--egress-allow") {
            const char* value = take_value(argc, argv, i, o, "--egress-allow");
            if (!value) break;
            o.egress_allow.emplace_back(value);
        } else if (arg == "-h" || arg == "--help") {
            o.show_help = true;
        } else {
            std::fprintf(stderr, "zetta: unknown option '%s'\n", arg.c_str());
            o.bad_usage = true;
            break;
        }
    }
    // Peers refuse a HELLO whose endpoints break the #771 bounds, so a bad
    // --advertise is refused here, where the operator sees it, instead of
    // silently cutting this node off from the cluster.
    for (const std::string& endpoint : o.cluster.advertise) {
        if (!zetta::cluster::is_valid_peer_endpoint(endpoint)) {
            std::fprintf(stderr,
                         "zetta: --advertise wants HOST:PORT endpoints; '%s' is not one\n",
                         endpoint.c_str());
            o.bad_usage = true;
        }
    }
    if (o.cluster.advertise.size() > zetta::cluster::kMaxAdvertisedEndpoints) {
        std::fprintf(stderr, "zetta: --advertise takes at most %zu endpoints\n",
                     zetta::cluster::kMaxAdvertisedEndpoints);
        o.bad_usage = true;
    }
    return o;
}

bool apply_pubsub_environment(Options& options) {
    auto apply_size = [&](const char* name, bool set_on_command_line,
                          std::size_t& destination) {
        if (set_on_command_line) return true;
        const char* value = std::getenv(name);
        if (value == nullptr) return true;
        if (parse_positive_size(value, destination)) return true;
        std::fprintf(stderr, "zetta: %s wants a positive number\n", name);
        return false;
    };
    if (!options.pubsub_dispatch_mode_from_cli) {
        if (const char* value = std::getenv("ZETTA_PUBSUB_DISPATCH_MODE")) {
            const auto mode = zetta::pubsub::parse_dispatch_mode(value);
            if (!mode) {
                std::fprintf(stderr,
                             "zetta: ZETTA_PUBSUB_DISPATCH_MODE wants pool, auto, per_key, or inline\n");
                return false;
            }
            options.node.pubsub_dispatch_mode = *mode;
        }
    }
    if (!apply_size("ZETTA_PUBSUB_TOPICS_PER_THREAD",
                    options.pubsub_topics_per_thread_from_cli,
                    options.node.pubsub_topics_per_thread)) {
        return false;
    }
    if (!apply_size("ZETTA_PUBSUB_DISPATCH_THREADS",
                    options.pubsub_dispatch_min_threads_from_cli,
                    options.node.pubsub_dispatch_min_threads)) {
        return false;
    }
    if (!options.pubsub_dispatch_max_threads_from_cli) {
        if (const char* value = std::getenv("ZETTA_PUBSUB_DISPATCH_MAX_THREADS")) {
            std::size_t maximum = 0;
            if (!parse_positive_size(value, maximum)) {
                std::fprintf(stderr,
                             "zetta: ZETTA_PUBSUB_DISPATCH_MAX_THREADS wants a positive number\n");
                return false;
            }
            options.node.pubsub_dispatch_max_threads = maximum;
        }
    }
    if (options.node.pubsub_dispatch_max_threads &&
        *options.node.pubsub_dispatch_max_threads <
            options.node.pubsub_dispatch_min_threads) {
        std::fprintf(stderr,
                     "zetta: pub/sub dispatch maximum threads must be at least the minimum\n");
        return false;
    }
    return true;
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

// `zetta pull <owner/model> [--revision REV] [--dir PATH]` — download open-weights
// model files natively (config + tokenizer + safetensors, sharded or single) into
// PATH, using the engine's own HTTPS client (no Python, no curl; ADR-17). The
// printed directory is what `ZETTA_QWEN_DIR` / the base-agent training consume.
int run_pull(int argc, char** argv) {
    std::string repo;
    std::string revision = "main";
    std::string dir;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (arg == "--revision" && i + 1 < argc) {
            revision = argv[++i];
        } else if (!arg.empty() && arg.front() == '-') {
            std::fprintf(stderr, "zetta pull: unknown option %s\n", argv[i]);
            return 2;
        } else if (repo.empty()) {
            repo = argv[i];
        } else {
            std::fprintf(stderr, "zetta pull: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (repo.empty()) {
        std::fprintf(stderr,
                     "usage: zetta pull <owner/model> [--revision REV] [--dir PATH]\n"
                     "  download open-weights model files (config + tokenizer +\n"
                     "  safetensors) into PATH (default ./models/<model>); set HF_TOKEN\n"
                     "  in the environment for a gated/private repo\n");
        return 2;
    }
    if (dir.empty()) {
        const std::string base = repo.substr(repo.find_last_of('/') + 1);
        dir = (std::filesystem::path("models") / base).string();
    }
    const char* token_env = std::getenv("HF_TOKEN");  // never logged; a secret header
    const std::string token = token_env != nullptr ? token_env : "";

    // `zetta pull` is a command a person runs to download named weights, not
    // graph execution: its process may reach Hugging Face (and its CDN, which
    // the resolve URL redirects to) on 443, plus whatever ZETTA_EGRESS_ALLOW
    // adds. Everything else stays denied (CTL-4, #75).
    {
        std::optional<zetta::connect::EgressPolicy> policy =
            configured_egress_policy({}, {});
        if (!policy) return 2;
        policy->merge(*zetta::connect::EgressPolicy::parse(
            "huggingface.co:443,*.huggingface.co:443,*.hf.co:443"));
        zetta::connect::set_egress_policy(std::move(*policy));
    }
    zetta::api::install_native_http_transport();
    std::fprintf(stderr, "pulling %s (%s) -> %s\n", repo.c_str(), revision.c_str(),
                 dir.c_str());
    const zetta::connect::ModelFetchResult result = zetta::connect::fetch_hf_model(
        zetta::connect::default_http_transport(), repo, revision, dir, token,
        [](const std::string& file, const std::string& note) {
            std::fprintf(stderr, "  %s: %s\n", file.c_str(), note.c_str());
        });
    zetta::api::shutdown_native_http_transport();

    if (!result.ok) {
        std::fprintf(stderr, "zetta pull: %s\n", result.reason.c_str());
        return 1;
    }
    std::fprintf(stderr, "done: %zu files\n", result.files.size());
    std::printf("%s\n", dir.c_str());  // the directory to point ZETTA_QWEN_DIR at
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "pull") {
        return run_pull(argc, argv);
    }
    Options opts = parse_args(argc, argv);
    if (opts.bad_usage) {
        print_usage(argv[0]);
        return 2;
    }
    if (opts.show_help) {
        print_usage(argv[0]);
        return 0;
    }

#ifdef ZETTA_WITH_POSTGRES
    // The libpqxx Postgres connector (#892) — registers into the connector runner
    // runner so a "postgres" source connection can run its read-only query. Built
    // only when the libpqxx submodule is present; without it, a Postgres query
    // fails cleanly ("the Postgres connector is not built into this binary").
    zetta::connect::install_postgres_protocol();
#endif
    // The native MySQL connector (#895) — the protocol is implemented in
    // zetta-core (connect/mysql_client.hpp); this installs its TCP/TLS driver so a
    // "mysql" source connection can run its read-only query. No vendor library,
    // so every build has it.
    zetta::connect::install_mysql_protocol();
#ifdef ZETTA_WITH_MLX
    // The fast Metal-GPU Qwen decode (#555) — register MlxQwenInference as the agent's
    // serving-inference runner, so chat()/respond() decode on the Apple-Silicon GPU
    // (one fused MLX graph per token, KV cache on-GPU) instead of the per-op-synced
    // autograd CPU path. Built only in the MLX build; without it the agent falls back
    // to the native CPU inference cleanly.
    zetta::compute::model::install_mlx_qwen_inference();
#endif

    // Provisioning env (#749): a burst/provisioned node is handed its cluster
    // coordinates as environment by the provisioner contract (ZETTA_SEED_PEER,
    // ZETTA_CLUSTER_TOKEN, ZETTA_DATA_DIR; docs/plans/cloud-provisioning.md Gap
    // A). Fold them in as defaults beneath the flags already parsed — explicit
    // flag > env > default — so a provisioned node auto-joins the secure (mTLS)
    // cluster with no manual flags. Inter-node mTLS is always on (#799).
    {
        zetta::cloud::NodeBootstrap bootstrap;
        bootstrap.seeds = opts.cluster.seeds;
        bootstrap.join_token = opts.join_token;
        bootstrap.data_dir = opts.node.data_dir;
        bootstrap.peer_from_cli = !opts.cluster.seeds.empty();
        bootstrap.join_token_from_cli = !opts.join_token.empty();
        bootstrap.data_dir_from_cli = opts.data_dir_from_cli;

        const zetta::cloud::BootstrapEnvApplied applied =
            zetta::cloud::apply_bootstrap_env(bootstrap);
        // The token is in `bootstrap` now; no child process inherits it (#770).
        zetta::cloud::scrub_bootstrap_secrets();
        for (const std::string& warning : applied.warnings) {
            std::fprintf(stderr, "zetta: warning: %s\n", warning.c_str());
        }
        if (!applied.error.empty()) {
            std::fprintf(stderr, "zetta: provisioning env rejected: %s\n",
                         applied.error.c_str());
            return 1;
        }

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
                // Name the source: a token left in an inherited environment makes
                // this node join that token's cluster instead of founding its own.
                summary += " " + applied.join_token_source +
                           "=**** (--join-token; joins the cluster this token identifies)";
            }
            if (!applied.data_dir.empty()) {
                summary += " ZETTA_DATA_DIR=" + applied.data_dir + " (--data-dir)";
            }
            std::printf("%s\n", summary.c_str());
        }
    }

    // The Studio web app bundle (#883): the image bakes the bundle and seats
    // ZETTA_WEBAPP_DIR; an explicit --webapp-dir always wins.
    if (opts.webapp_dir.empty()) {
        if (const char* env = std::getenv("ZETTA_WEBAPP_DIR")) opts.webapp_dir = env;
    }
    if (opts.clerk_issuer.empty()) {
        if (const char* env = std::getenv("ZETTA_CLERK_ISSUER")) opts.clerk_issuer = env;
    }
    // The origins a Clerk token may be issued to (#1423): this node's Studio origins, comma
    // separated. With an issuer and none, every Clerk token is refused.
    if (const char* env = std::getenv("ZETTA_CLERK_AUTHORIZED_PARTIES")) {
        std::vector<std::string> parties;
        std::string_view rest(env);
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            std::string party(rest.substr(0, comma));
            std::erase_if(party, [](unsigned char c) { return std::isspace(c) != 0; });
            if (!party.empty()) parties.push_back(std::move(party));
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        zetta::api::set_clerk_authorized_parties(std::move(parties));
    }
    // Consent enforcement is off unless asked for, by flag or by environment —
    // a provisioned node has no argv to edit (#995). An explicit --governance
    // has already set it; the env can only turn it ON, never back off.
    if (!opts.node.enforce_consent) {
        if (const char* env = std::getenv("ZETTA_GOVERNANCE")) {
            const std::string value = env;
            opts.node.enforce_consent = value == "1" || value == "true" ||
                                        value == "yes" || value == "on";
        }
    }
    if (!apply_pubsub_environment(opts)) {
        return 2;
    }
    {
        auto list = opts.connector_allow_destinations;
        if (!list) {
            if (const char* environment = std::getenv("ZETTA_CONNECTOR_ALLOW_DESTINATIONS"))
                list = environment;
        }
        std::string error;
        const auto allowed =
            zetta::connect::ConnectorDestinations::parse(list.value_or(""), &error);
        if (!allowed) {
            std::fprintf(stderr, "zetta: --connector-allow-destinations: %s\n", error.c_str());
            return 2;
        }
        zetta::connect::set_connector_destinations(*allowed);
    }
    // Where an author-chosen action or report delivery endpoint may reach (#1394):
    // public addresses only, plus the classes named here. An explicit flag wins
    // over the environment; an unknown class stops the node.
    {
        std::optional<std::string> list = opts.action_allow_destinations;
        if (!list) {
            if (const char* env = std::getenv("ZETTA_ACTION_ALLOW_DESTINATIONS")) list = env;
        }
        std::string error;
        const auto allowed = zetta::connect::AllowedDestinations::parse(list.value_or(""), &error);
        if (!allowed) {
            std::fprintf(stderr, "zetta: --action-allow-destinations: %s\n", error.c_str());
            return 2;
        }
        zetta::connect::set_allowed_destinations(*allowed);
        std::printf("zetta: action delivery destinations: %s\n", allowed->describe().c_str());
    }
    std::optional<zetta::connect::EgressPolicy> egress_policy =
        configured_egress_policy(opts.egress_allow, opts.clerk_issuer);
    if (!egress_policy) {
        return 2;
    }
    if (!opts.audit_retention_from_cli) {
        if (const char* environment =
                std::getenv("ZETTA_AUDIT_RETENTION_DAYS")) {
            std::uint32_t days = 0;
            if (!parse_uint32(environment, days)) {
                std::fprintf(stderr,
                             "zetta: ZETTA_AUDIT_RETENTION_DAYS wants a non-negative number\n");
                return 2;
            }
            opts.node.audit_retention = std::chrono::hours(24ULL * days);
        }
    }
    if (!opts.audit_summary_retention_from_cli) {
        if (const char* environment =
                std::getenv("ZETTA_AUDIT_SUMMARY_RETENTION_DAYS")) {
            std::uint32_t days = 0;
            if (!parse_uint32(environment, days)) {
                std::fprintf(
                    stderr,
                    "zetta: ZETTA_AUDIT_SUMMARY_RETENTION_DAYS wants a non-negative number\n");
                return 2;
            }
            opts.node.audit_summary_retention =
                std::chrono::hours(24ULL * days);
        }
    }
    if (const char* environment =
            std::getenv("ZETTA_MALLOC_TRIM_THRESHOLD_BYTES")) {
        std::size_t threshold = 0;
        if (!parse_positive_size(environment, threshold)) {
            std::fprintf(
                stderr,
                "zetta: ZETTA_MALLOC_TRIM_THRESHOLD_BYTES wants a positive number\n");
            return 2;
        }
        opts.node.malloc_trim_threshold_bytes = threshold;
    }

    // One UUID is both the cluster node id and the replication actor id, so a
    // node's changes are attributed to it and merge conflict-free. It's stable
    // across restarts (persisted in data_dir/node_id) so the change history
    // doesn't fragment and membership keys don't accumulate. Replication is the
    // source of truth for data; it must outlive the Node (which captures into and
    // reconciles from it) and the Cluster (whose thread gossips it).
    const std::string node_id = load_or_create_node_id(opts.node.data_dir);

    // Inter-node transport security (CTL-2, #466): always on (#799). The first
    // start generates a join token into data_dir/security/ (or adopts
    // --join-token) and mints this node's identity; the Cluster then refuses any
    // connection that can't prove that token's CA. There is no plaintext mode —
    // every accepted or dialed peer link runs mutual TLS 1.3.
    {
        std::string reason;
        std::optional<zetta::cluster::ClusterSecurity> security =
            zetta::cluster::load_or_create_cluster_security(
                opts.node.data_dir, node_id, opts.join_token, &reason);
        if (!security) {
            std::fprintf(stderr, "zetta: cluster security setup failed: %s\n",
                         reason.c_str());
            return 1;
        }
        // Saved connector credentials are sealed under a key derived from the
        // join token (#1230): every member holds the token outside the
        // replicated catalogue, so every member opens what another sealed.
        // The retired tokens let a rotated node re-seal what the old one sealed.
        opts.node.connection_root_secret = security->join_token;
        opts.node.retired_connection_root_secrets = security->retired_join_tokens;
        opts.cluster.security = std::move(*security);
    }

    // The owner's admin token (#733 / #744): --admin-token, else ZETTA_ADMIN_TOKEN.
    // We log WHERE the token lives, never its value.
    std::string admin_secret = opts.admin_token;
    if (admin_secret.empty()) {
        if (const char* env = std::getenv("ZETTA_ADMIN_TOKEN")) admin_secret = env;
    }
    if (opts.identity_root.empty()) {
        if (const char* env = std::getenv("ZETTA_IDENTITY_ROOT")) opts.identity_root = env;
    }
    // The lowest identity epoch this node accepts (#1360): set on a node joining
    // a cluster whose epoch the operator recorded, so an older epoch replayed to
    // it is refused even when no node that held a newer one is left.
    if (opts.identity_min_epoch.empty()) {
        if (const char* env = std::getenv("ZETTA_IDENTITY_MIN_EPOCH")) {
            opts.identity_min_epoch = env;
        }
    }
    if (!opts.identity_min_epoch.empty()) {
        std::uint64_t min_epoch = 0;
        const std::string& text = opts.identity_min_epoch;
        const auto [pointer, error] =
            std::from_chars(text.data(), text.data() + text.size(), min_epoch);
        if (error != std::errc{} || pointer != text.data() + text.size()) {
            std::fprintf(stderr, "zetta: --identity-min-epoch wants a number\n");
            return 1;
        }
        opts.node.identity_min_epoch = min_epoch;
    }

    // The identity root (#763 / #764, docs/governance/identity-seal.md): who may
    // write accounts, roles, tokens and policies. Membership (the join token) is
    // not authority: every identity record is sealed by the root and verified by
    // every node before it is honored. The root derives from the owner's admin
    // token, so every node given the cluster's token can issue identity and a
    // node started with any other token cannot. Pinned in data-dir on first start.
    {
        std::string reason;
        zetta::governance::IdentityRootSetup root =
            zetta::governance::load_or_create_identity_root(
                opts.node.data_dir, admin_secret, opts.identity_root, &reason);
        if (!root.authority) {
            std::fprintf(stderr, "zetta: identity root setup failed: %s\n",
                         reason.c_str());
            return 1;
        }
        if (!root.warning.empty()) {
            std::fprintf(stderr, "zetta: WARNING: %s\n", root.warning.c_str());
        }
        if (root.pinned_now && root.authority->can_seal() && admin_secret.empty() &&
            !opts.cluster.seeds.empty()) {
            std::fprintf(stderr,
                         "zetta: WARNING: joining with neither --admin-token nor "
                         "--identity-root: this node made its own identity root and "
                         "will not honor the cluster's accounts. Restart it on a "
                         "fresh data dir with the --identity-root the first node "
                         "prints.\n");
        }
        std::printf("zetta: identity root %s (%s)\n",
                    root.authority->public_key_hex().c_str(),
                    root.authority->can_seal() ? "this node issues identity"
                                               : "verify-only");
        // The join command carries the root's public key (not a secret), so the
        // next node pins this cluster's root (Cluster::peer_command).
        opts.cluster.identity_root = root.authority->public_key_hex();
        opts.node.identity_authority = std::move(root.authority);
        // A rotation (#1342) the node has not followed yet: it takes the admin
        // token's root once the replicated successions reach it.
        opts.node.identity_owner_candidate = std::move(root.owner_candidate);
        opts.node.identity_previous_roots = std::move(root.previous_roots);
    }

    zetta::cluster::ReplicationManager replication(node_id);

    zetta::Node node(opts.node, &replication);

    // Outbound egress is denied by default (CTL-4, #75): the node may connect
    // only to loopback and the destinations its operator allowed. Refusals land
    // on the audit trail as egress_denied.
    zetta::connect::set_egress_policy(std::move(*egress_policy));
    std::printf("zetta: egress %s\n",
                zetta::connect::egress_policy().describe().c_str());
    EgressDenialAudit egress_denial_audit(node);

    // Keep the native coroutine HTTPS client alive only while the Node is
    // alive. This guard joins the worker before Node destruction on every
    // return path, including refused startup.
    NativeHttpTransportLifetime native_http_transport;

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

    // Bootstrap the owner (#733 / #744 / #763) AFTER joining. A node that is
    // joining an existing cluster has an empty store until the first state
    // exchange, so deciding "is there an owner yet?" from its own disk before it
    // connects mints a second owner. With seeds configured, wait (bounded) until a
    // peer's document matches ours, reconcile it, and only then look. Seeding is
    // also idempotent under a root: the owner, its token and assignment, and the
    // built-in roles have ids fixed by the root, so nodes that seed at once — or a
    // founder whose peers were not up within the wait — converge to one owner.
    {
        if (!opts.cluster.seeds.empty()) {
            constexpr auto kJoinSyncWait = std::chrono::seconds(10);
            const auto deadline = std::chrono::steady_clock::now() + kJoinSyncWait;
            bool synced = false;
            while (!synced && std::chrono::steady_clock::now() < deadline) {
                for (const zetta::cluster::NodeStatus& status : cluster.nodes()) {
                    if (!status.is_self && status.replication.in_sync) synced = true;
                }
                if (!synced) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::printf("zetta: %s\n",
                        synced ? "joined: in sync with a peer before seeding identity"
                               : "no peer in sync within 10s: seeding identity as a "
                                 "founder (idempotent under the identity root)");
        }
        node.tick();  // reconcile what the join brought in
        // A joiner whose root is not the cluster's honors none of the cluster's
        // accounts (and they none of its). Say so plainly, once, instead of
        // leaving every request to 401 with no reason.
        {
            std::size_t foreign = 0;
            const zetta::governance::IdentityAuthority* root = node.identity_authority();
            for (const auto& [key, value] : replication.snapshot().items()) {
                if (!zetta::governance::is_identity_key(key) || !value.is_object() ||
                    !value.contains(std::string(zetta::governance::kIdentitySealField))) {
                    continue;
                }
                if (!root->open(key, value)) ++foreign;
            }
            if (foreign > 0) {
                std::fprintf(stderr,
                             "zetta: WARNING: %zu identity record(s) in the cluster are "
                             "sealed by another identity root, so this node does not "
                             "honor them. Every node of one cluster needs the same "
                             "--admin-token, or the --identity-root the first node "
                             "prints (docs/governance/identity-seal.md).\n",
                             foreign);
            }
        }

        // The default-deny gate is always on (#799), so a fresh node with the MCP
        // server enabled needs an owner token or every request 401s. If no secret
        // was supplied, generate one and print it ONCE so the node is never locked
        // out (#733) — in the operator path a secret is always injected, so this
        // fires only for a bare local run. Decided after the join, so a joining
        // node that received the cluster's owner never mints its own.
        bool generated = false;
        if (admin_secret.empty() && opts.mcp_enabled && node.user_count() == 0 &&
            node.can_issue_identity()) {
            admin_secret = zetta::governance::generate_token_secret();
            generated = true;
        }
        // Reconcile the built-in admin/writer/reader roles on EVERY startup — not
        // only fresh-store bootstrap (bootstrap_owner early-returns once the user
        // store is seeded). The roles live in the replicated store, seeded once at
        // cluster creation; without this a binary upgrade leaves them frozen at
        // whatever permission vocabulary first seeded them, so an upgraded cluster
        // whose 'admin' role predates a newly-added capability keeps denying it
        // (e.g. orchestration:testsuite_run). Idempotent — writes only on drift; a
        // verify-only node writes nothing.
        node.ensure_builtin_roles();
        if (!admin_secret.empty()) {
            const zetta::Node::BootstrapResult boot =
                node.bootstrap_owner(admin_secret);
            if (boot.performed && generated) {
                std::printf(
                    "zetta: no --admin-token supplied — generated the owner '%s' "
                    "access token (id %s). SAVE THIS — it is shown only once:\n"
                    "    %s\n"
                    "    authenticate MCP/REST with: Authorization: Bearer <token>\n",
                    boot.user_name.c_str(), boot.token_id.c_str(),
                    admin_secret.c_str());
            } else if (boot.performed) {
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
        }
    }

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
    // The join token is the cluster root secret (it derives the CA key and seals
    // saved connector credentials), and this line lands in pod logs and log
    // pipelines: peer_command() names where the token goes, never its value
    // (#1393). The line above says where to read it.
    std::printf("zetta: add another node with: %s\n",
                cluster.peer_command().c_str());
    // Flush the startup lines now: under a container runtime stdout is a pipe, so
    // they would otherwise sit in the buffer until it fills.
    std::fflush(stdout);

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
    // injected. The token is a secret: it rides the same bootstrap environment as the
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
        api_options.webapp_dir = opts.webapp_dir;
        api_options.clerk_issuer = opts.clerk_issuer;
        // The default-deny gate is always on (#799) and has no toggle: every
        // HTTP/MCP/REST path needs a bearer that resolves to a user via the token
        // store, else 401. There is no opt-out and no legacy shared-secret bypass.
        // The in-process GUI console is unaffected (it authorizes locally).
        api_server = std::make_unique<zetta::api::ApiServer>(node, &cluster,
                                                             api_options);
        if (api_server->start()) {
            std::printf("zetta: MCP endpoint http://%s:%u/mcp "
                        "(auth required — bearer must resolve to a user)\n",
                        api_options.listen_address.c_str(),
                        static_cast<unsigned>(api_server->port()));
            if (node.token_count() == 0) {
                std::fprintf(stderr,
                             "zetta: WARNING no tokens exist — every request "
                             "will 401. Seed an owner with --admin-token <secret> "
                             "or ZETTA_ADMIN_TOKEN (#799/#733).\n");
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
        // Work a pool worker queues on the owner (an offloaded call's gate, the
        // pub/sub authorizer) wakes the idle loop instead of waiting a tick (#1063).
        node.set_owner_wakeup([server = api_server.get()] { server->wake(); });
    }
    const auto drain_api = [&api_server, &node] {
        if (api_server) api_server->drain();
        // Work pool workers queued on the owner (an offloaded call's gate, the
        // pub/sub authorizer) runs on this wake too, not only inside the 10 Hz
        // tick: otherwise each such hop costs up to a full tick (#1063).
        node.drain_owner_work();
    };
    // The owning loop's idle wait — wakes the instant a request is enqueued so
    // the drain above runs immediately instead of on the next 10 Hz tick (#731).
    const auto wait_api = [&api_server](std::chrono::milliseconds max_idle) {
        if (api_server) api_server->wait_for_work(max_idle);
    };

    int rc = 0;
    if (opts.mode == Mode::Headless) {
        rc = zetta::run_headless(node, cluster, drain_api, wait_api);
    } else {
        zetta::gui::App app(node, cluster);
        app.set_tick_hook(drain_api);
        // Wake the frame loop on enqueue (#731): run_on_owner pushes an SDL event
        // so App::run's SDL_WaitEventTimeout returns early and drains immediately.
        if (api_server) api_server->set_wakeup([&app] { app.request_wake(); });
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

    node.set_owner_wakeup({});  // before the API server can go away
    if (api_server) api_server->stop_accepting();
    // Wake receive_events long polls before waiting for their connection
    // coroutines. ApiServer::stop keeps both owner queues draining until the
    // I/O thread has unwound, then the io_context can be destroyed safely
    // (#1077).
    if (node.pubsub()) node.pubsub()->shutdown();
    if (api_server) {
        api_server->stop();
        api_server.reset();
    }
    cluster.stop();
    node.stop();
    return rc;
}
