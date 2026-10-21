#include "zetta/api/mcp_docs.hpp"

#include <string>
#include <string_view>

namespace zetta::api {

std::string render_mcp_md(std::string_view origin, bool token_required) {
    const std::string endpoint = std::string(origin) + "/mcp";
    std::string md;
    md.reserve(2048);

    md += "# Connect to this Zetta node over MCP\n\n";
    md += "This node speaks the [Model Context Protocol](https://modelcontextprotocol.io) "
          "over **streamable HTTP** (JSON-RPC request/response — no SSE). The endpoint is:\n\n";
    md += "```\nPOST " + endpoint + "\n```\n\n";

    if (token_required) {
        md += "## Authentication\n\n";
        md += "A **bearer token is required**. It is provisioned as a Kubernetes secret by the "
              "operator and is **not shown here** — read it from the secret (ask your operator "
              "for the exact name/namespace):\n\n";
        md += "```sh\nkubectl get secret <cluster>-zetta-mcp -n <namespace> "
              "-o jsonpath='{.data.token}' | base64 -d\n```\n\n";
        md += "Keep it out of your shell history:\n\n";
        md += "```sh\nread -rs ZETTA_MCP_TOKEN   # paste the token, then Enter\nexport ZETTA_MCP_TOKEN\n```\n\n";
        md += "### Claude Code\n\n```sh\nclaude mcp add --transport http zetta " + endpoint +
              " \\\n  --header \"Authorization: Bearer $ZETTA_MCP_TOKEN\"\n```\n\n";
        md += "### Claude Desktop / claude.ai\n\n"
              "Settings -> Connectors -> **Add custom connector** -> URL `" + endpoint +
              "`, then add the request header `Authorization: Bearer <token>`.\n\n";
        md += "### Smoke test (raw JSON-RPC)\n\n```sh\ncurl -s " + endpoint +
              " \\\n  -H \"Authorization: Bearer $ZETTA_MCP_TOKEN\" \\\n  -H 'Content-Type: application/json' \\\n"
              "  -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
              "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
              "\"clientInfo\":{\"name\":\"you\",\"version\":\"0\"}}}'\n```\n";
    } else {
        md += "## Authentication\n\n";
        md += "**None** — this endpoint is unauthenticated (local or trusted-network use only). "
              "Do not expose it to the public internet without a token.\n\n";
        md += "### Claude Code\n\n```sh\nclaude mcp add --transport http zetta " + endpoint + "\n```\n\n";
        md += "### Claude Desktop / claude.ai\n\n"
              "Settings -> Connectors -> **Add custom connector** -> URL `" + endpoint + "`.\n\n";
        md += "### Smoke test (raw JSON-RPC)\n\n```sh\ncurl -s " + endpoint +
              " \\\n  -H 'Content-Type: application/json' \\\n"
              "  -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
              "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
              "\"clientInfo\":{\"name\":\"you\",\"version\":\"0\"}}}'\n```\n";
    }

    md += "\n---\n\nOnce connected, the node exposes its capabilities as MCP tools "
          "(`tools/list`). The same surface is available as REST (`" + std::string(origin) +
          "/v1/openapi.json`) and for orientation as `" + std::string(origin) + "/llms.txt`.\n\n";
    md += "It also ships guided **prompts** (`prompts/list`) — reusable workflows over the "
          "tools your client surfaces as slash commands. Start with **`build_and_run`**: give "
          "it a plain-language ask and it has an agent build + run a real execution graph "
          "(e.g. churn) and report the result.\n";
    return md;
}

}  // namespace zetta::api

namespace zetta::api {

std::string render_cli_md(std::string_view origin) {
    std::string out;
    out += "# zetta — command line\n\n";
    out += "One binary. A *node* is one running instance; every node is an equal\n";
    out += "peer, so there is no separate server or client command.\n\n";
    out += "```sh\nzetta [options]\n```\n\n";

    out += "## The two that are most often confused\n\n";
    out += "| Flag | What it binds |\n|---|---|\n";
    out += "| `--listen HOST:PORT` | the **cluster peer** port (default 7777) |\n";
    out += "| `--mcp-listen HOST:PORT` | the **API, MCP and web UI** (default 127.0.0.1:4800) |\n\n";
    out += "`--listen` does **not** move the API. Setting it and then calling the\n";
    out += "port you chose returns nothing, which reads exactly like a dead node.\n\n";
    out += "This node serves its API at `";
    out += std::string(origin);
    out += "`.\n\n";

    out += "## Running\n\n| Flag | Meaning |\n|---|---|\n";
    out += "| `--headed` | run with the SDL3 + ImGui UI (default) |\n";
    out += "| `--headless` | run without a UI, for servers with no display |\n";
    out += "| `--data-dir PATH` | the database directory |\n";
    out += "| `--no-preflight` | skip the startup data integrity check |\n";
    out += "| `--log-cluster` | log cluster gossip to stderr (debugging) |\n";
    out += "| `--governance` | enforce consent gating on cloud-touching calls (off by default) |\n";
    out += "| `--action-allow-destinations LIST` | address classes an action or report delivery endpoint may reach besides public ones: `loopback`, `private`, `link-local`, `metadata` (default none) |\n";
    out += "| `--audit-retention-days N` | keep ordinary audit rows for N days; zero disables expiration |\n";
    out += "| `--audit-summary-retention-days N` | keep audit summaries for N days (default 365); zero disables expiration |\n";
    out += "| `--egress-allow LIST` | outside destinations this node may connect to (repeatable): host, `*.domain`, address or CIDR range, each optionally `:port`; `*` allows everything |\n";
    out += "| `-h`, `--help` | this reference, on the terminal |\n\n";
    out += "**Outbound network is denied by default.** Without `--egress-allow` the\n";
    out += "node may connect only to loopback (and its Clerk issuer, when one is set);\n";
    out += "every other connection is refused before DNS and recorded as an\n";
    out += "`egress_denied` audit event. See\n";
    out += "[`docs/governance/egress-policy.md`](docs/governance/egress-policy.md).\n\n";
    out += "**Consent gating is off by default.** A consent-gated capability\n";
    out += "(`create_dataset_from_query`, `cloud_inspect`, `detect_provisioners`,\n";
    out += "a cloud `add_nodes`) executes on the first call, and the ungated call\n";
    out += "is recorded as a `consent_bypassed` audit event naming what ran.\n";
    out += "`--governance` restores the challenge: the first call issues a\n";
    out += "one-time consent request that a **different** principal must approve.\n\n";
    out += "**Action delivery reaches public addresses only.** An action or report\n";
    out += "delivery endpoint is chosen by its author, so the node resolves it and\n";
    out += "refuses loopback, private, link-local and metadata addresses unless\n";
    out += "`--action-allow-destinations` names the class. See\n";
    out += "[`docs/governance/destination-policy.md`](docs/governance/destination-policy.md).\n\n";

    out += "## Pub/sub dispatch\n\n| Flag | Meaning |\n|---|---|\n";
    out += "| `--pubsub-dispatch-mode MODE` | node-wide mode: `pool` (default), `auto`, `per_key` or `inline` |\n";
    out += "| `--pubsub-topics-per-thread N` | active topic and inbox keys per pool worker (default 250) |\n";
    out += "| `--pubsub-dispatch-threads N` | minimum shared-pool worker count (default 1) |\n";
    out += "| `--pubsub-dispatch-max-threads N` | maximum shared-pool worker count (default detected CPU count) |\n\n";
    out += "The default pool grows as active keys cross each topics-per-thread boundary and\n";
    out += "shrinks with hysteresis. See [`docs/pubsub/dispatch.md`](docs/pubsub/dispatch.md).\n\n";

    out += "## Cluster\n\n| Flag | Meaning |\n|---|---|\n";
    out += "| `--peer HOST:PORT` | a node to connect to. Repeatable; all peers are equal |\n";
    out += "| `--join-token TOKEN` | the cluster join token, printed by the first node |\n";
    out += "| `--advertise H:P` | extra reachable endpoints to advertise (NAT / load balancer) |\n";
    out += "| `--dial-out-only` | no routable address: reach peers outbound, never be dialed |\n";
    out += "| `--node-type T` | `permanent` (default) or `ephemeral` burst capacity |\n";
    out += "| `--port-range N` | bind the first free port in [PORT, PORT+N) |\n\n";
    out += "**Inter-node traffic is always mTLS.** There is no plaintext mode.\n\n";

    out += "## API, MCP and the web app\n\n| Flag | Meaning |\n|---|---|\n";
    out += "| `--mcp-listen H:P` | where the API, MCP and web UI bind |\n";
    out += "| `--no-mcp` | disable the embedded MCP server |\n";
    out += "| `--webapp-dir PATH` | serve the built Studio web app at `/app` |\n";
    out += "| `--admin-token T` | seed the owner token on first boot |\n";
    out += "| `--identity-root KEY` | pin the cluster's identity root (printed by the first node) |\n";
    out += "| `--clerk-issuer URL` | verify Clerk JWTs; omit for bearer-only, on-prem |\n\n";
    out += "Without `--admin-token`, a fresh node generates the owner token and\n";
    out += "prints it **once**, so a node is never locked out. It is hashed at rest\n";
    out += "and never logged again.\n\n";
    out += "Every node of one cluster gets the **same** admin token: it derives the\n";
    out += "cluster's identity root, which seals every account, role and token.\n";
    out += "A node started with another token, or a member that writes identity\n";
    out += "into the replicated catalogue itself, issues nothing any node honors.\n\n";

    out += "## Provisioning by environment\n\n";
    out += "Applied as defaults and overridden by the flags above, so a provisioned\n";
    out += "node auto-joins with no manual flags.\n\n";
    out += "| Variable | Maps to |\n|---|---|\n";
    out += "| `ZETTA_SEED_PEER` | `--peer` (comma or space separated) |\n";
    out += "| `ZETTA_CLUSTER_TOKEN_FILE` | `--join-token`, read from a file (preferred) |\n";
    out += "| `ZETTA_CLUSTER_TOKEN` | `--join-token` |\n";
    out += "| `ZETTA_ADMIN_TOKEN` | `--admin-token` |\n";
    out += "| `ZETTA_IDENTITY_ROOT` | `--identity-root` |\n";
    out += "| `ZETTA_GOVERNANCE` | `--governance` (`1`/`true`/`yes`/`on`) |\n";
    out += "| `ZETTA_ACTION_ALLOW_DESTINATIONS` | `--action-allow-destinations` |\n";
    out += "| `ZETTA_EGRESS_ALLOW` | `--egress-allow` (used when no flag is given) |\n";
    out += "| `ZETTA_PUBSUB_DISPATCH_MODE` | `--pubsub-dispatch-mode` |\n";
    out += "| `ZETTA_PUBSUB_TOPICS_PER_THREAD` | `--pubsub-topics-per-thread` |\n";
    out += "| `ZETTA_PUBSUB_DISPATCH_THREADS` | `--pubsub-dispatch-threads` |\n";
    out += "| `ZETTA_PUBSUB_DISPATCH_MAX_THREADS` | `--pubsub-dispatch-max-threads` |\n";
    out += "| `ZETTA_AUDIT_RETENTION_DAYS` | `--audit-retention-days` |\n";
    out += "| `ZETTA_AUDIT_SUMMARY_RETENTION_DAYS` | `--audit-summary-retention-days` |\n\n";

    out += "## See also\n\n";
    out += "- [`/mcp.md`](/mcp.md) — connect an MCP client to this node\n";
    out += "- [`/llms.txt`](/llms.txt) — the documentation index\n";
    out += "- [`/v1/openapi.json`](/v1/openapi.json) — the REST surface\n";
    return out;
}

}  // namespace zetta::api
