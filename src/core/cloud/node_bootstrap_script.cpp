#include "zetta/cloud/node_bootstrap_script.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace zetta::cloud {

namespace {

// Characters a POSIX shell word may hold without quoting.
bool needs_no_quoting(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           std::string_view{"_@%+=:,./-"}.find(character) != std::string_view::npos;
}

bool has_control_character(std::string_view value) {
    for (const char character : value) {
        const auto code = static_cast<unsigned char>(character);
        if (code < 0x20 || code == 0x7f) return true;
    }
    return false;
}

std::string_view store_label(ClusterTokenStore store) {
    switch (store) {
        case ClusterTokenStore::none: return "none";
        case ClusterTokenStore::aws_ssm_parameter: return "aws-ssm-parameter";
        case ClusterTokenStore::azure_key_vault: return "azure-key-vault";
        case ClusterTokenStore::gcp_secret_manager: return "gcp-secret-manager";
        case ClusterTokenStore::oci_vault: return "oci-vault";
    }
    return "none";
}

// The shared head of every script: the values, logging, and two helpers.
constexpr std::string_view kHelpers = R"SH(
log() {
    echo "zetta-bootstrap: $*" >&2
    if command -v logger >/dev/null 2>&1; then logger -t zetta-bootstrap -- "$*"; fi
    return 0
}

# The first "<name>": "<string>" field of the JSON document on stdin.
json_field() {
    sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -n 1
}

# Write an Authorization header to a private file and print its path, so an
# access token is never an argument of curl (argv is readable by every local
# user). printf is a shell builtin: the token is never exec'd.
bearer_header_file() {
    header_file=$(mktemp) || return 1
    printf 'Authorization: Bearer %s\n' "$1" > "$header_file"
    printf '%s' "$header_file"
}
)SH";

// Azure: a Key Vault access token from the instance metadata endpoint for the
// user-assigned managed identity, then GET the secret (REST, `value` field).
constexpr std::string_view kFetchAzure = R"SH(
fetch_token() {
    set -- -fsS -G -H 'Metadata: true' \
        --data-urlencode 'api-version=2018-02-01' \
        --data-urlencode 'resource=https://vault.azure.net'
    if [ -n "$TOKEN_IDENTITY" ]; then
        set -- "$@" --data-urlencode "msi_res_id=$TOKEN_IDENTITY"
    fi
    access=$(curl "$@" 'http://169.254.169.254/metadata/identity/oauth2/token' | json_field access_token)
    [ -n "$access" ] || return 1
    header_file=$(bearer_header_file "$access") || return 1
    unset access
    curl -fsS -H "@$header_file" \
        "https://$TOKEN_CONTAINER.vault.azure.net/secrets/$TOKEN_SECRET?api-version=7.4" | json_field value
    rm -f "$header_file"
}
)SH";

// GCP: an access token for the instance's service account from the metadata
// server, then Secret Manager's `versions/latest:access` (base64 payload).
constexpr std::string_view kFetchGcp = R"SH(
fetch_token() {
    access=$(curl -fsS -H 'Metadata-Flavor: Google' \
        'http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token' | json_field access_token)
    [ -n "$access" ] || return 1
    header_file=$(bearer_header_file "$access") || return 1
    unset access
    payload=$(curl -fsS -H "@$header_file" \
        "https://secretmanager.googleapis.com/v1/projects/$TOKEN_CONTAINER/secrets/$TOKEN_SECRET/versions/latest:access" | json_field data)
    rm -f "$header_file"
    [ -n "$payload" ] || return 1
    printf '%s' "$payload" | base64 -d
}
)SH";

// OCI: the `oci` CLI with instance-principal auth reads the secret bundle by
// name (base64 content).
constexpr std::string_view kFetchOci = R"SH(
fetch_token() {
    set -- secrets secret-bundle get-secret-bundle-by-name --auth instance_principal \
        --vault-id "$TOKEN_CONTAINER" --secret-name "$TOKEN_SECRET" \
        --query 'data."secret-bundle-content".content' --raw-output
    if [ -n "$TOKEN_REGION" ]; then set -- "$@" --region "$TOKEN_REGION"; fi
    payload=$(oci "$@") || return 1
    printf '%s' "$payload" | base64 -d
}
)SH";

// AWS: the `aws` CLI with the instance profile decrypts the SecureString.
constexpr std::string_view kFetchAws = R"SH(
fetch_token() {
    set -- ssm get-parameter --name "$TOKEN_SECRET" --with-decryption \
        --query Parameter.Value --output text
    if [ -n "$TOKEN_REGION" ]; then set -- "$@" --region "$TOKEN_REGION"; fi
    aws "$@"
}
)SH";

// Fetch (retrying while an IAM grant propagates), validate, write the 0400 file,
// write the environment file, start the engine.
constexpr std::string_view kMain = R"SH(
write_env_file() {
    tmp_env=$(mktemp "$ENV_FILE.XXXXXX") || return 1
    {
        if [ -n "$ZETTA_SEED_PEER" ]; then printf 'ZETTA_SEED_PEER=%s\n' "$ZETTA_SEED_PEER"; fi
        if [ "$TOKEN_STORE" != none ]; then printf 'ZETTA_CLUSTER_TOKEN_FILE=%s\n' "$TOKEN_FILE"; fi
    } > "$tmp_env"
    chmod 0644 "$tmp_env"
    mv -f "$tmp_env" "$ENV_FILE"
}

for directory in "$(dirname "$TOKEN_FILE")" "$(dirname "$ENV_FILE")"; do
    if ! mkdir -p "$directory" || ! chmod 0755 "$directory"; then
        log "cannot create $directory"
        exit 1
    fi
done

if [ "$TOKEN_STORE" != none ]; then
    if ! id zetta >/dev/null 2>&1; then
        useradd --system --no-create-home --shell /usr/sbin/nologin zetta 2>/dev/null ||
            log "there is no zetta user and useradd failed; $TOKEN_FILE stays owned by root"
    fi
    attempt=1
    while :; do
        token=$(fetch_token 2>/dev/null | tr -d ' \t\r\n')
        if [ "${#token}" -eq 64 ]; then
            case $token in *[!0-9a-f]*) ;; *) break ;; esac
        fi
        if [ "$attempt" -ge "$TOKEN_ATTEMPTS" ]; then
            log "no valid cluster join token in $TOKEN_STORE secret $TOKEN_SECRET after $attempt attempts; check this VM's grant on it (docs/cloud/node-bootstrap-script.md)"
            exit 1
        fi
        log "waiting for the cluster join token ($TOKEN_STORE secret $TOKEN_SECRET), attempt $attempt"
        attempt=$((attempt + 1))
        sleep "$TOKEN_RETRY_SECONDS"
    done
    tmp_token=$(mktemp "$TOKEN_FILE.XXXXXX") || { log "cannot stage $TOKEN_FILE"; exit 1; }
    printf '%s\n' "$token" > "$tmp_token"
    unset token
    if id zetta >/dev/null 2>&1; then chown zetta "$tmp_token"; fi
    chmod 0400 "$tmp_token"
    mv -f "$tmp_token" "$TOKEN_FILE"
fi

write_env_file || { log "cannot write $ENV_FILE"; exit 1; }

if command -v systemctl >/dev/null 2>&1 && systemctl cat zetta.service >/dev/null 2>&1; then
    systemctl enable zetta.service >/dev/null 2>&1
    systemctl restart zetta.service
    log "started zetta.service"
else
    log "zetta.service is not on this image; $ENV_FILE is staged for it"
fi
)SH";

void assign(std::string& script, std::string_view name, std::string_view value) {
    script += name;
    script += '=';
    script += shell_quote(value);
    script += '\n';
}

}  // namespace

std::string cluster_token_secret_name(std::string_view pool_name) {
    return std::string{pool_name} + "-cluster-token";
}

std::string shell_quote(std::string_view value) {
    if (value.empty()) return "''";
    bool plain = true;
    for (const char character : value) {
        if (!needs_no_quoting(character)) {
            plain = false;
            break;
        }
    }
    if (plain) return std::string{value};
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
}

std::optional<std::string> render_node_bootstrap_script(
    const NodeBootstrapScriptOptions& options, std::string* reason) {
    const auto fail = [&](std::string message) -> std::optional<std::string> {
        if (reason != nullptr) *reason = std::move(message);
        return std::nullopt;
    };

    const ClusterTokenReference& token = options.token;
    for (const std::string_view value : {std::string_view{options.seed_peer},
                                         std::string_view{token.secret_name},
                                         std::string_view{token.container},
                                         std::string_view{token.identity},
                                         std::string_view{token.region},
                                         std::string_view{options.token_path},
                                         std::string_view{options.env_path}}) {
        if (has_control_character(value)) {
            return fail("a node bootstrap value holds a control character");
        }
    }

    std::string_view fetch;
    switch (token.store) {
        case ClusterTokenStore::none:
            break;
        case ClusterTokenStore::aws_ssm_parameter:
            fetch = kFetchAws;
            break;
        case ClusterTokenStore::azure_key_vault:
            if (token.container.empty()) return fail("Azure needs the Key Vault name");
            if (token.identity.empty()) {
                return fail("Azure needs the managed identity the VM reads the secret with");
            }
            fetch = kFetchAzure;
            break;
        case ClusterTokenStore::gcp_secret_manager:
            if (token.container.empty()) return fail("GCP needs the project id");
            fetch = kFetchGcp;
            break;
        case ClusterTokenStore::oci_vault:
            if (token.container.empty()) return fail("OCI needs the vault OCID");
            fetch = kFetchOci;
            break;
    }
    if (options.token_path.empty() || options.env_path.empty()) {
        return fail("the token and environment file paths must be set");
    }
    if (token.store != ClusterTokenStore::none && token.secret_name.empty()) {
        return fail("the token store needs a secret name");
    }

    std::string script =
        "#!/bin/sh\n"
        "# zetta node bootstrap (#1316), written by the burst control plane.\n"
        "# It carries no secret: the cluster join token is fetched at boot from the\n"
        "# vendor secret store with this VM's own identity, written to a mode-0400\n"
        "# file the engine reads through ZETTA_CLUSTER_TOKEN_FILE, and never put in\n"
        "# user-data, an argument or the environment.\n"
        "set -u\n"
        "umask 077\n";
    assign(script, "ZETTA_SEED_PEER", options.seed_peer);
    assign(script, "TOKEN_STORE", store_label(token.store));
    assign(script, "TOKEN_SECRET", token.secret_name);
    assign(script, "TOKEN_CONTAINER", token.container);
    assign(script, "TOKEN_IDENTITY", token.identity);
    assign(script, "TOKEN_REGION", token.region);
    assign(script, "TOKEN_FILE", options.token_path);
    assign(script, "ENV_FILE", options.env_path);
    assign(script, "TOKEN_ATTEMPTS", std::to_string(options.attempts == 0 ? 1 : options.attempts));
    assign(script, "TOKEN_RETRY_SECONDS", std::to_string(options.retry_delay_seconds));
    script += kHelpers;
    script += fetch;
    script += kMain;
    return script;
}

}  // namespace zetta::cloud
