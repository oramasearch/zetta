# Cluster transport security — mTLS + per-node identity + the join token

Blueprint **CTL-2** for trust boundary **TB-3** (the inter-node boundary), #466:
every inter-node connection runs **mutual TLS 1.3**, every node holds a
**cryptographic identity** (its own keypair + certificate), and joining the
cluster is **gated on a join token**. Built on Corosio's OpenSSL TLS provider
over the same equal-peer transport ([cluster.md](cluster.md)); generic TLS and
framing live in [`zetta::transport`](transport-library.md), while the
cluster-specific surface is `zetta::cluster::ClusterSecurity` and
`ClusterConfig::security`.

## Trust model — the join token is the trust root

A cluster has one **join token**: 32 CSPRNG bytes, printed as 64 lowercase hex
characters. Everything else derives from it:

1. **The cluster CA is derived deterministically from the token.** An Ed25519
   private key *is* a 32-byte seed, so `SHA256(token-bytes ‖ "zetta-cluster-ca-v1")`
   gives every token holder the **same CA keypair** — and, with all certificate
   fields fixed (subject `CN=zetta-cluster-ca`, a fixed 2026→2076 validity
   window, serial from the subject hash), the same CA certificate. No CA ever
   travels over the network and no enrollment RPC exists: **possession of the
   token is the ability to mint a cluster-trusted identity**, which matches the
   equal-peer, no-leader architecture (any member could enroll others anyway).
2. **Each node mints its own identity**: a fresh random Ed25519 keypair in a
   CA-signed certificate whose `CN` is the node id, persisted next to the token
   (below) so the identity is stable across restarts.
3. **The transport enforces it**: `Cluster::start()` asks
   `zetta::transport::make_mutual_tls_context` to verify the PEM bundle and
   build one context — this node's identity + the cluster CA as the **only**
   trust anchor, `require_peer` verification in **both** directions, TLS 1.3
   only, no
   hostname check (membership is proven by the CA, not by an address). The TLS
   handshake runs **before** any `ZETTA/1` protocol line, so an unenrolled or
   plaintext dialer is dropped before it even reaches `HELLO`. Credentials that
   fail to load **fail closed** — the node runs no networking rather than
   falling back to plaintext.
4. **One shared framed TLS stream, serialized I/O.** A link's read pump and
   write pump share one `zetta::transport::FramedStream` over an OpenSSL stream,
   whose `SSL` object + scratch buffers cannot be driven concurrently
   (interleaved `SSL_read`/`SSL_write` corrupt the record stream). The reusable
   transport's async mutex serializes them: the read pump waits for socket
   readability *off* the lock, then drains the decrypted bytes *under* it, so a
   read and a write never overlap on the one `SSL` object (#786). A plaintext link
   keeps lock-free full-duplex I/O. Gossip currently re-ships the full document
   each round (incremental sync is #794), so this serialization is half-duplex per
   link under heavy write load.

Mixed modes fail cleanly in both directions: a TLS node treats a plaintext
`HELLO` as a failed handshake; a plaintext node treats a TLS `ClientHello` as a
malformed line. Either way the connection drops and nothing joins.

## Joining a cluster

The first node generates the token on first start and prints where it is and
the command to join, with the token masked:

```bash
# First node — generates data_dir/security/, prints:
#   zetta: inter-node mTLS on (join token in .../security/cluster-secret.json)
#   zetta: add another node with: zetta --peer 192.168.1.10:7777 --join-token <join-token>
zetta

# Another machine joins with that command, the token read from the file above
# (or passed as ZETTA_CLUSTER_TOKEN):
zetta --peer 192.168.1.10:7777 --join-token <64-hex>
```

**The token is never printed or logged (#1393).** It derives the cluster CA key
and is the connector-credential sealing root (#1230), so anyone holding it can
mint a trusted peer identity. `Cluster::peer_command()` (the headless banner and
the Cluster panel's text) puts `kJoinTokenPlaceholder` (`<join-token>`) where the
token goes. Only `Cluster::peer_command_with_join_token()` returns the clear
token, and its one caller is the headed Cluster panel's **Copy command** button,
an explicit operator action. A refused secret file or `--join-token` names the
file and the field, never the value. The process test
`tests/cluster/test_startup_log_redaction.cpp` starts the real binary with the
token from `--join-token`, from `ZETTA_CLUSTER_TOKEN` and generated on first
start, and fails if any output line contains it. A cluster whose token reached
logs before this change should be re-keyed (pass a new `--join-token`; the old
one is kept as a retired token so sealed connector credentials still open).

`--join-token` is persisted in the joiner's own data dir on first use, so it is
needed only once. Passing a *different* token to a node that already has one
**re-keys** it: the new token wins, is persisted, and the node's identity is
re-minted under the new CA. Inter-node mTLS is **always on** — there is **no
plaintext peer mode**. The old `--cluster-plaintext` opt-out was **removed**
(#799): it disabled the whole subsystem and was a silent-downgrade security
hole. The only way to run an unsecured `Cluster` now is to embed a bare
`Cluster` with an all-empty `ClusterSecurity` in a test or host process — no CLI
flag or env selects it.

### Provisioned / burst nodes (#670)

A cloud-burst node can't be handed these flags interactively, so the provisioner
interface injects them at boot. Each `<Vendor>ControlPlane` puts the cluster's seed
address in **`ZETTA_SEED_PEER`** and — when the cluster is secure — its join token
in **`ZETTA_CLUSTER_TOKEN`**, through the same per-cloud delivery path (an AWS instance
tag / Azure cloud-init custom-data / GCP + OCI metadata / a Kubernetes pod env / a
container `--env`), so the node's bootstrap comes up with the equivalent of
`zetta --peer <seed> --join-token <token>` and joins the **mTLS** cluster (the
only kind — inter-node mTLS is always on, #799). Both are injected once, at fleet
wire time, from this node's own live cluster security — `main.cpp` passes
`ClusterConfig::security.join_token` to `ProvisionerFleet::wire`, which fills any
per-cloud config that didn't set its own. The token is the cluster secret, so
unlike the seed peer it is **never** part of the config's JSON boundary: it is not
serialized into the node's detected provisioner config or the `system.provisioners`
table, and rides only the bootstrap path. A `Cluster` with empty security (an
embedded bare `Cluster` in a test/host, the only unsecured path — #799) has an
empty token, so nothing is injected. (The reference control planes carry the token illustratively
through the same visible path as the seed; a production control plane sources it from a
per-cloud secret store rather than a describe-able tag / literal.)

## On disk — `<data_dir>/security/`

| File | What | Mode |
|---|---|---|
| `cluster-secret.json` | `{"version": 1, "join_token": "<64 hex>", "retired_join_tokens"?: ["<64 hex>", …]}` — schema-validated on load (`cluster_secret_schema()`, generated `schemas/cluster_secret.schema.json`); a malformed file **fails the start with a reason**, it is never silently clobbered. A `--join-token` that replaces the stored token moves it to `retired_join_tokens` (newest first, at most four) | 0600 |
| `connection-root-secret` | only on a node embedded **without** a join token (the library, a test): the 64-hex root secret its saved connector credentials are sealed under (#1230) | 0600 |
| `node-cert.pem` | this node's CA-signed identity certificate (`CN` = node id) | 0600 |
| `node-key.pem` | this node's Ed25519 private key | 0600 |

The stored identity is reused while it still verifies against the derived CA
and carries this node's id; a token change or a node-id mismatch re-mints it.

The join token is also the root secret of the **connector-credential sealing
key** (#1230): `main` passes it as `NodeConfig::connection_root_secret`, and saved
connections' credentials are sealed under an HKDF-SHA256 key derived from it, so
every member opens what another sealed and the key is never in the replicated
catalogue. The retired tokens let a re-keyed node open what was sealed under the
previous one; its next start re-seals those entries under the current token. See
[connector query](../data/connector-query.md#the-credential-is-sealed-at-rest-1230).

## Code map

- `include/zetta/cluster/security.hpp` — `ClusterSecurity` (the PEM bundle +
  token), `generate_join_token` / `valid_join_token`,
  `derive_cluster_security` (pure), `load_or_create_cluster_security`
  (persistence), `cluster_secret_schema()`.
- `include/zetta/transport/` — the independent public transport surface:
  verified PEM credentials, mutual TLS 1.3 context creation, and the framed
  Corosio stream. See [transport-library.md](transport-library.md).
- `src/transport/` — the `zetta_transport` implementation: OpenSSL credential
  validation and the shared framing/TLS I/O coordination (#1283).
- `src/core/cluster/security.cpp` — the **one TU allowed to name OpenSSL
  libcrypto for cluster identity generation** (key generation, X509 minting,
  PEM). Generic PEM verification lives in `src/transport/security.cpp`.
- `src/core/cluster/cluster.cpp` — cluster connection lifecycle and protocol;
  it creates the shared context and wraps every socket in `FramedStream` before
  HELLO. The read-pump + link-writer pattern and line bytes are unchanged.
- `tests/cluster/test_cluster_security.cpp` — token/derivation/persistence
  units + real-TCP mTLS convergence, wrong-token rejection, plaintext
  rejection.

## Threat model + known limitations

**Closed** (TB-3 vs outsiders): eavesdropping and tampering on inter-node
traffic (TLS 1.3), uninvited joins (no token → no CA-signed cert → handshake
fails), silent plaintext downgrade (fail-closed credential loading, no
fallback path).

**Open, by design of this slice** (follow-up: #669 enrollment/pinning/revocation):

- **Member-vs-member node-id impersonation.** Corosio exposes no
  peer-certificate accessor, so the `HELLO` node id cannot be checked against
  the peer's certificate CN; and with the CA key derivable from the token,
  any member can mint a certificate for any CN anyway. Fixing both means a
  CSR-based enrollment (nodes keep private keys the CA never sees; an existing
  member signs) + certificate pinning — the natural V2 on top of this wire
  format, requiring an upstream peer-cert accessor (#669).
- **No revocation.** Removing a node's trust today means rotating the token
  (re-key every node). Token rotation is manual (`--join-token` on each node);
  each node re-seals the saved connector credentials on its first start under
  the new token.
**Closed by #670:** burst nodes now receive the join token too — the provisioner
interface injects `ZETTA_CLUSTER_TOKEN` alongside `ZETTA_SEED_PEER`, so a provisioned
node maps it with `ZETTA_SEED_PEER` to the CLI defaults, derives the cluster CA
and identity, and joins a secure cluster with no manual flags (see "Provisioned
/ burst nodes" above and [node bootstrap](../cloud/node-bootstrap-env.md)).
