# The identity root: sealed identity records (#763, #764, #1341)

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

A sealed value carries two extra fields:

- `_v`, its **version**: a positive integer, larger for every later record of the same key (#1341);
- `_seal`: `"ed25519-v2:<128 hex>"`, the root's Ed25519 signature over `zetta-identity-seal-v2`, the
  root's public key, the key, and the value without `_seal`, `_v` included (nlohmann's ordered dump,
  so every node builds the same bytes).

The seal binds the value to its key, its version and its root: a sealed record copied to another key,
or given another version, does not open. A record with no `_v` does not open either.

A **removal** is a sealed record of its own, a tombstone: `{"_tombstone": true, "_v": N, "_seal":
"..."}` and nothing else. The CRDT's own removal marker (`{"_removed": true}`) carries no seal, so a
node never treats it as a removal of an identity record.

## How a node uses it

- **Writing.** Every identity write in `Node` goes through `Node::replicate_identity`, which seals
  the value before it enters the document (`Principal::create_key` seals access keys the same way).
  A node whose root is **verify-only** refuses every identity write (`Node::identity_writable`):
  `create_user`, `create_role`, `grant_permissions`, `create_token`, `assign_role`,
  `create_policy`, `create_principal`, their drops and revokes, `ensure_builtin_roles` and
  `bootstrap_owner` all return their failure value and write nothing.
- **Removing.** Every identity removal in `Node` goes through `Node::remove_identity`, which writes
  a sealed tombstone (`Principal::drop_key` does the same for access keys). Dropping a user, a role
  or a principal writes one tombstone per record it removes (its tokens, its assignments, its keys).
- **Reading.** The identity reconcilers never read the raw document. `Node::trusted_identity` hands
  them, for each identity key, the **newest sealed record this node has accepted**, with `_seal` and
  `_v` removed; a tombstone is handed over as absence. See [Versions](#versions-and-removals-1341).
  A record with no seal, another root's seal, a changed field or a moved key is never materialized.

## Versions and removals (#1341)

The CRDT document converges on the last write to each key, whoever made it. A seal alone says the
root issued a value once, not that it is the current one, so before versions a member that kept an
earlier sealed value (a token before its revocation, an assignment before its removal) could write it
back and every node honored it again; and a member could remove any identity record with the CRDT's
unsealed tombstone. Versions close both.

- **Choosing a version.** A write or a removal takes `next_identity_version`: larger than the version
  of the record the document holds now, larger than the one the node holds, and at least the wall
  clock in milliseconds, so issuers on different nodes that never saw each other's records still
  order by time. Two records of one key and one version (two issuers at the same millisecond) are
  ordered by their seal text, so every node picks the same one (`identity_record_supersedes`).
- **Holding the newest.** Each node keeps, per identity key, the newest sealed record it has accepted
  (`Node::identity_held_`). When the document's record opens and is not older, it becomes the held
  one. When the document holds an older sealed record, one that does not open, or nothing (the CRDT's
  tombstone), the node keeps its held record and **writes it back** to the document, so a node that
  joins later receives the newer record and not the replay. Only an identical sealed value is ever
  written back, so this settles in one round and never loops.
- **Across restarts.** The held records are kept in `<data_dir>/security/identity-held.json`
  (`{"version": 1, "records": {"<key>": <sealed value>}}`), written atomically whenever they change
  and on stop, and loaded in `Node::start` before the first reconcile. Each record in it is verified
  again on load, so the file is only as trusted as its seals; one that does not open is ignored. A
  file that does not parse refuses the start, rather than forgetting what the node held.

So a member that replays an old sealed value changes nothing anyone honors, and a member that deletes
an identity record from the document, or writes a tombstone of its own, deletes nothing. Tampering
with a sealed record neither widens it nor removes it: the node keeps the record it holds.

A node that has **never** seen the newer record and is told only the older one (it was offline, and
every node that held the newer one is gone) accepts the older one: it holds nothing to compare
against. That needs every node that saw the change to be lost at once.

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
- **A node that never saw a change** accepts an older sealed record when no node that holds the newer
  one is left to write it back (see [Versions](#versions-and-removals-1341)).
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
- `src/core/node.cpp` — `replicate_identity`, `remove_identity`, `trusted_identity`,
  `hold_identity`, `load_identity_held`/`persist_identity_held`, `identity_writable`,
  `seal_preexisting_identity`, `seeded_identity_id`, `bootstrap_owner`.
- `src/core/governance/principal.cpp` — sealed access keys and their sealed removal.
- `src/main.cpp` — root setup, seeding after the join, the join command with `--identity-root`.
- Tests: `tests/governance/test_identity_seal.cpp` (seal, versions, tombstones, pin, node
  behaviour, the held records across a restart, upgrade),
  `tests/cluster/test_cluster_join_authority.cpp` (two nodes over mutual TLS: #848 steps 4 and 5, and
  #1341: a member replaying a revoked token and a removed assignment, and deleting identity with
  unsealed removals),
  `tests/governance/test_authz_regressions.cpp` (`AuthzCluster.Finding763_*`, `Finding764_*`).
