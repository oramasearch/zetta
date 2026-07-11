# Cluster transport security — mTLS + per-node identity + the join token

Blueprint **CTL-2** for trust boundary **TB-3** (the inter-node boundary), #466:
every inter-node connection runs **mutual TLS 1.3**, every node holds a
**cryptographic identity** (its own keypair + certificate), and joining the
cluster is **gated on a join token**. Built on Corosio's OpenSSL TLS provider
over the same equal-peer transport ([cluster.md](cluster.md)); the surface is
`zetta::cluster::ClusterSecurity` (`include/zetta/cluster/security.hpp`) and
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
3. **The transport enforces it**: `Cluster::start()` builds one TLS context —
   this node's identity + the cluster CA as the **only** trust anchor,
   `require_peer` verification in **both** directions, TLS 1.3 minimum, no
   hostname check (membership is proven by the CA, not by an address). The TLS
   handshake runs **before** any `ZETTA/1` protocol line, so an unenrolled or
   plaintext dialer is dropped before it even reaches `HELLO`. Credentials that
   fail to load **fail closed** — the node runs no networking rather than
   falling back to plaintext.

Mixed modes fail cleanly in both directions: a TLS node treats a plaintext
`HELLO` as a failed handshake; a plaintext node treats a TLS `ClientHello` as a
malformed line. Either way the connection drops and nothing joins.

## Joining a cluster

The first node generates the token on first start; `peer_command()` (the
Cluster panel's copy line, and the headless printout) carries it:

```bash
# First node — generates data_dir/security/, prints:
#   zetta: inter-node mTLS on (join token in .../security/cluster-secret.json)
#   zetta: add another node with: zetta --peer 192.168.1.10:7777 --join-token <64-hex>
zetta

# Another machine joins with that exact command:
zetta --peer 192.168.1.10:7777 --join-token <64-hex>
```

`--join-token` is persisted in the joiner's own data dir on first use, so it is
needed only once. Passing a *different* token to a node that already has one
**re-keys** it: the new token wins, is persisted, and the node's identity is
re-minted under the new CA. `--cluster-plaintext` disables the whole subsystem
(debugging only — it prints a warning, and such a node cannot talk to a TLS
cluster); it conflicts with `--join-token`.

## On disk — `<data_dir>/security/`

| File | What | Mode |
|---|---|---|
| `cluster-secret.json` | `{"version": 1, "join_token": "<64 hex>"}` — schema-validated on load (`cluster_secret_schema()`, generated `schemas/cluster_secret.schema.json`); a malformed file **fails the start with a reason**, it is never silently clobbered | 0600 |
| `node-cert.pem` | this node's CA-signed identity certificate (`CN` = node id) | 0600 |
| `node-key.pem` | this node's Ed25519 private key | 0600 |

The stored identity is reused while it still verifies against the derived CA
and carries this node's id; a token change or a node-id mismatch re-mints it.

## Code map

- `include/zetta/cluster/security.hpp` — `ClusterSecurity` (the PEM bundle +
  token), `generate_join_token` / `valid_join_token`,
  `derive_cluster_security` (pure), `load_or_create_cluster_security`
  (persistence), `cluster_secret_schema()`.
- `src/core/cluster/security.cpp` — the **one TU allowed to name OpenSSL
  libcrypto** (key generation, X509 minting, PEM), the way `cluster.cpp` is the
  one TU naming Corosio.
- `src/core/cluster/cluster.cpp` — the transport: the TLS context in
  `Cluster::State`, the per-link `openssl_stream` (via `zetta::runtime::net`
  aliases), the line protocol over a type-erased `any_stream` (plaintext or
  TLS behind one code path). The read-pump + link-writer full-duplex pattern is
  preserved — the TLS session supports exactly one in-flight read + one
  in-flight write.
- `tests/cluster/test_cluster_security.cpp` — token/derivation/persistence
  units + real-TCP mTLS convergence, wrong-token rejection, plaintext
  rejection.

## Threat model + known limitations

**Closed** (TB-3 vs outsiders): eavesdropping and tampering on inter-node
traffic (TLS 1.3), uninvited joins (no token → no CA-signed cert → handshake
fails), silent plaintext downgrade (fail-closed credential loading, no
fallback path).

**Open, by design of this slice** (follow-ups: #669 enrollment/pinning/revocation,
#670 burst-provisioner token plumbing):

- **Member-vs-member node-id impersonation.** Corosio exposes no
  peer-certificate accessor, so the `HELLO` node id cannot be checked against
  the peer's certificate CN; and with the CA key derivable from the token,
  any member can mint a certificate for any CN anyway. Fixing both means a
  CSR-based enrollment (nodes keep private keys the CA never sees; an existing
  member signs) + certificate pinning — the natural V2 on top of this wire
  format, requiring an upstream peer-cert accessor (#669).
- **No revocation.** Removing a node's trust today means rotating the token
  (re-key every node). Token rotation is manual (`--join-token` on each node).
- **Burst nodes** (cloud provisioners) now **consume** `ZETTA_CLUSTER_TOKEN`
  (and `ZETTA_SEED_PEER`) from the environment on boot — the binary maps them to
  `--join-token` / `--peer` as defaults beneath explicit flags, so a provisioned
  node whose env carries the token derives the cluster CA + its mTLS identity and
  auto-joins the secure cluster with no manual flags (#749;
  [../cloud/node-bootstrap-env.md](../cloud/node-bootstrap-env.md)). The
  remaining gap is the **injection** side: the provisioner seam still hands the
  VM only `ZETTA_SEED_PEER` today, so until the join-token plumbing lands
  (#670/#742) a burst node's env carries no token and it comes up unable to join
  a TLS cluster. The consuming side is ready for it.
