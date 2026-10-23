# The identity root: sealed identity records (#763, #764)

**Membership is not authority.** The join token ([../cluster/security.md](../cluster/security.md))
lets a node into the cluster's replicated document. It does not let the node decide who the users
are, which roles they hold, or which tokens open the cluster. Those records are **sealed** by the
cluster's **identity root**, and every node checks the seal before it honors a record.

Before this, any member, or anything that could write the replicated document, could add an
`owner` token every node honored (#764), and a node joining with its own `--admin-token` seeded a
second `owner` that spread to the cluster (#763). Both are covered by #848 (*an unauthorized node
cannot join or orchestrate*, steps 4 and 5).

## What is sealed

Every record under the seven identity prefixes of the catalogue:

| Key | Record |
|---|---|
| `user:<id>` | a user |
| `role:<id>` | a role and its permissions |
| `token:<id>` | an access or API token (salted hash, never the secret) |
| `user_role:<id>` | a role assignment |
| `policy:<id>` | a row-level security policy |
| `principal:<id>` | a legacy principal |
| `key:<id>` | a legacy access key |

A sealed value carries one extra field, `_seal`: `"ed25519-v1:<128 hex>"`, the root's Ed25519
signature over `zetta-identity-seal-v1`, the key, and the value without `_seal` (nlohmann's ordered
dump, so every node builds the same bytes). The seal binds the value to its key: a sealed record
copied to another key does not open.

## How a node uses it

- **Writing.** Every identity write in `Node` goes through `Node::replicate_identity`, which seals
  the value before it enters the document (`Principal::create_key` seals access keys the same way).
  A node whose root is **verify-only** refuses every identity write (`Node::identity_writable`):
  `create_user`, `create_role`, `grant_permissions`, `create_token`, `assign_role`,
  `create_policy`, `create_principal`, their drops and revokes, `ensure_builtin_roles` and
  `bootstrap_owner` all return their failure value and write nothing.
- **Reading.** The identity reconcilers never read the raw document. `Node::trusted_identity` hands
  them only the records whose seal opens, with `_seal` removed. A record with no seal, another
  root's seal, a changed field or a moved key is **absent**: it is never materialized, and a copy
  already on disk is dropped. Tampering with a sealed record therefore removes it; it never widens
  it.

## Where the root comes from

`zetta::governance::load_or_create_identity_root` (`include/zetta/governance/identity_seal.hpp`)
decides it at startup and pins it in `<data_dir>/security/identity-root.json`:

| On the first start the node has | It pins | It can issue identity |
|---|---|---|
| `--admin-token` / `ZETTA_ADMIN_TOKEN` | the root **derived** from the token: Ed25519 seeded by `SHA-256("zetta-identity-root-v1\n" + token)`; only the public key is written | yes |
| `--identity-root KEY` / `ZETTA_IDENTITY_ROOT` | that public key | only if its admin token derives the same key |
| neither | a **generated** root; its seed is stored (file mode 0600) | yes |

After the first start **the pinned root wins**. A later `--admin-token` that derives another key
leaves the node **verify-only** and logs a warning. A later `--identity-root` that differs from the
pin refuses the start. A malformed file refuses the start and is never overwritten. The schema is
`identity_root_schema()`, generated to [`schemas/identity_root.schema.json`](../../schemas/identity_root.schema.json).

So, by deployment:

- **Under the Orama operator** every node gets the same `ZETTA_ADMIN_TOKEN`, derives the same root,
  and issues identity. Nothing changes for the operator.
- **A bare cluster**: the first node prints the root with the join command:
  `zetta: add another node with: zetta --peer <addr> --join-token <token> --identity-root <key>`.
  The root is a public key, not a secret. A joiner given it is verify-only unless it also has the
  owner token.
- **A joiner with neither** makes its own root. It honors none of the cluster's accounts and the
  cluster honors none of its records; the node logs a warning saying so.
- **A joiner with a different `--admin-token`** derives a different root. Whatever it seeds is
  sealed by that root, so no other node honors it (#763).

## Seeding after the join (#763)

`main.cpp` seeds the owner and reconciles the built-in roles **after** the cluster starts. With
seeds configured it waits up to 10 seconds for a peer whose document matches its own, ticks the node
to reconcile what arrived, and only then asks whether the user store is empty. Seeding is also
idempotent under a root: the owner, its token, its `admin` assignment and the built-in roles have ids
fixed by the root (`IdentityAuthority::derived_id`, an RFC 9562 version-8 UUID). Nodes of one root
that seed at once, or a founder whose peers were not up within the wait, write the same keys and
converge to one owner and one role per built-in name. Without a root (tests) the ids stay random.

## Upgrading a node that already has accounts

On the first start under a root, `Node::seal_preexisting_identity` seals the unsealed identity
records already in the node's **own persisted document**. It runs in `Node::start`, before the
cluster connects, so nothing a peer sends is included. It then writes
`<data_dir>/security/identity-sealed` and never runs again, so a record that arrives unsealed later
is never sealed by this node. A verify-only node seals nothing and waits for a node that holds the
root. Records that already carry a seal are left alone.

**Upgrade all nodes together**, as the audit trail already requires
([audit-trail.md](audit-trail.md)). A node from before the root rejects a token record that carries
`_seal` (its parser refuses unknown fields), so while old and new binaries run side by side the old
nodes drop every sealed token and answer 401. Stop the old nodes, upgrade every node, restart.

## What it does not cover

- **A node handed the owner token is the owner.** It holds the root's private key and can issue
  anything. Under the operator every node has the token, so a compromised node can still issue
  identity. Narrowing that needs identity issued by a key no node holds, which is not built.
- **Removals carry no seal.** A removal is a CRDT tombstone, so a member can still delete an identity
  record: denial of service, never a grant. Deleting a row policy can widen what a user reads only
  while an empty policy admits every row (#765).
- **Replay.** A member that kept an earlier sealed value of a key (a token before its revocation)
  can write it again, and it opens. Revocation that survives replay needs a sealed version or
  tombstone that the reconciler orders; tracked as a follow-up.
- **Rotating the owner token does not move the root.** The pin wins, so after a rotation the nodes
  become verify-only (existing tokens keep working; no new identity can be issued) until the root is
  moved deliberately. Root rotation is a follow-up.
- **Provisioned burst nodes** get `ZETTA_SEED_PEER` and `ZETTA_CLUSTER_TOKEN` but not the root yet,
  so they make their own and honor none of the cluster's accounts. They cannot issue identity the
  cluster honors either. Passing `ZETTA_IDENTITY_ROOT` through the provisioners is a follow-up.
- **Node identity.** Any join-token holder can still mint a certificate for any node id (#1319) and
  there is no per-node revocation (#1320). Neither affects the seal, which does not depend on node
  certificates; they bound who can write the document at all.

## Code map

- `include/zetta/governance/identity_seal.hpp`, `src/core/governance/identity_seal.cpp` — the root,
  seal and open, derived ids, the pin file and its schema.
- `src/core/node.cpp` — `replicate_identity`, `trusted_identity`, `identity_writable`,
  `seal_preexisting_identity`, `seeded_identity_id`, `bootstrap_owner`.
- `src/core/governance/principal.cpp` — sealed access keys.
- `src/main.cpp` — root setup, seeding after the join, the join command with `--identity-root`.
- Tests: `tests/governance/test_identity_seal.cpp` (seal, pin, node behaviour, upgrade),
  `tests/cluster/test_cluster_join_authority.cpp` (two nodes over mutual TLS: #848 steps 4 and 5),
  `tests/governance/test_authz_regressions.cpp` (`AuthzCluster.Finding763_*`, `Finding764_*`).
