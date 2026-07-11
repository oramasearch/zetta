#pragma once

// Node bootstrap from provisioning environment (#749).
//
// A burst/provisioned node is handed its cluster coordinates as environment
// variables by the provisioner seam — `ZETTA_SEED_PEER` (the join address) and
// `ZETTA_CLUSTER_TOKEN` (the cluster join token), plus `ZETTA_DATA_DIR` for
// symmetry (see `src/core/cloud/*_cli_control_plane.cpp` for the injection side
// and `docs/plans/cloud-provisioning.md` Gap A). Nothing read those on boot, so
// a provisioned node could not auto-join. This maps them onto the same node
// config the CLI flags populate, so a provisioned node auto-joins a secure
// (mTLS) cluster with no manual flags.
//
// Precedence is **explicit CLI flag > env > default**: env only fills a gap the
// operator left. A plaintext cluster stays plaintext — env never forces
// security on (the token is taken from env only when neither `--join-token` nor
// `--cluster-plaintext` was given). Non-throwing (a zetta-core boundary).

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zetta::cloud {

// A pluggable lookup over the environment, so the mapping is unit-testable
// without mutating the process environment. Returns the value, or nullptr when
// the variable is unset (matching std::getenv).
using EnvReader = std::function<const char*(const char*)>;

// The default reader: std::getenv over the real process environment.
const char* process_env(const char* name);

// Split a `ZETTA_SEED_PEER` value into individual host:port seeds. The value may
// list several peers separated by commas and/or whitespace; blank tokens are
// dropped. (One seed is the common case; a list mirrors repeated `--peer`.)
std::vector<std::string> split_seed_peers(std::string_view value);

// The bootstrap fields the provisioning env can populate, together with which of
// them the operator already set explicitly on the CLI. `apply_bootstrap_env`
// fills only the fields left unset (`*_from_cli == false`).
struct NodeBootstrap {
    std::vector<std::string> seeds;   // --peer (repeatable) <- ZETTA_SEED_PEER
    std::string join_token;           // --join-token        <- ZETTA_CLUSTER_TOKEN
    std::string data_dir;             // --data-dir          <- ZETTA_DATA_DIR
    bool cluster_plaintext = false;   // --cluster-plaintext (env never sets this)

    bool peer_from_cli = false;
    bool join_token_from_cli = false;
    bool data_dir_from_cli = false;
};

// What the environment actually contributed — for the single startup log line
// (the token is logged redacted, so only the fact it came from env is kept).
// Empty when the environment changed nothing.
struct BootstrapEnvApplied {
    std::vector<std::string> seeds;   // seeds appended from ZETTA_SEED_PEER
    bool join_token = false;          // token taken from ZETTA_CLUSTER_TOKEN
    std::string data_dir;             // data_dir taken from ZETTA_DATA_DIR (else empty)

    bool any() const {
        return !seeds.empty() || join_token || !data_dir.empty();
    }
};

// Apply the provisioning env vars onto `config` as defaults beneath explicit CLI
// flags (explicit flag > env > default), and report what env contributed:
//   ZETTA_SEED_PEER     -> appends to config.seeds  (skipped if peer_from_cli)
//   ZETTA_CLUSTER_TOKEN -> sets config.join_token   (skipped if join_token_from_cli
//                                                     OR cluster_plaintext — env
//                                                     never forces security on)
//   ZETTA_DATA_DIR      -> sets config.data_dir     (skipped if data_dir_from_cli)
// Blank/unset env vars are ignored. Non-throwing.
BootstrapEnvApplied apply_bootstrap_env(NodeBootstrap& config,
                                        const EnvReader& read = process_env);

}  // namespace zetta::cloud
