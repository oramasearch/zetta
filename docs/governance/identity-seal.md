# The identity root: sealed identity records (#763, #764, #1341, #1342, #1360)

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
every node that held the newer one is gone) has nothing of its own to compare against. The identity
epoch closes that, below.

## The identity epoch (#1360)

**Decided under Steve's 2026-11-14 delegation:** a sealed, monotonic identity epoch that commits to
the version of every identity record (option 1, with the committed `(key, _v)` list held beside a
digest), rather than a separate revocation list (option 2 alone), because one record then bounds every
key at once: a revocation, a removed assignment and a changed role are all just newer versions.

- **The record.** `identity_epoch:cluster` holds `{"entries": {"<identity key>": <version>, ...},
  "digest": "<64 hex>", "issued_at": <ms>}`, sealed by the root like any identity record. Its `_v` is
  the **epoch number**. The digest is SHA-256 over `"<key>\n<version>\n"` for every entry in key order
  (`identity_epoch_digest`); an epoch whose digest does not match its entries, or that lists a key that
  is not an identity key, is not an epoch (`identity_epoch_entries`). Tombstones are committed too, at
  their version.
- **Who writes it.** A node that holds the root seals a new epoch over every record it holds after any
  identity write or removal, coalesced to at most once per second (`Node::maybe_write_identity_epoch`,
  `kIdentityEpochInterval`, from `tick`), and once soon after it starts. It takes its number from
  `next_identity_version`, so the number only grows. When a node holding the root accepts an epoch
  that misses a record it holds, or commits an older version of one, it seals the next epoch over its
  newer view, so concurrent writers converge.
- **Held like a record.** `is_identity_key` is true for it, so it is held in `identity-held.json`,
  refused when older than the one held, and written back like any identity record (#1341).
- **The accept rule.** `Node::trusted_identity` settles the epoch before any other key. A record of key
  K is accepted only at a version at least the one the held epoch commits to for K; an older one is
  refused, and a record the node itself holds at an older version is no longer trusted until the newer
  one arrives. Keys the epoch does not list (records written since) are decided as before. When a newer
  epoch outdates a record the node already trusted, every identity family is reconciled again
  (`Node::accept_identity_epoch`).
- **`--identity-min-epoch N`** (or `ZETTA_IDENTITY_MIN_EPOCH`, `NodeConfig::identity_min_epoch`): the
  node refuses every epoch below N and honors **no** identity record until it holds an epoch at least
  N. `get_identity_epoch` (cluster:admin) reads a live node's epoch number, so the operator records it
  and starts a joining node with it.

**HELLO does not carry the epoch.** The design proposed sending each node's epoch number in the
cluster `HELLO` and refusing a lower peer's identity records. Gossip merges documents rather than
attributing each record to the peer it came from, so there is no per-peer record to refuse; and the
held epoch already refuses an older epoch from any peer, and every record it outdates, whoever sends it.

**What is left (documented, closed operationally).** If every node that held epoch N **and** every
node holding the root are gone at once, an attacker with an older sealed epoch and the records it
committed could replay them to a brand-new cluster that holds nothing. `--identity-min-epoch` closes
that: a node started with the recorded floor refuses anything older.

## Where the root comes from

`zetta::governance::load_or_create_identity_root` (`include/zetta/governance/identity_seal.hpp`)
decides it at startup and pins it in `<data_dir>/security/identity-root.json`:

| On the first start the node has | It pins | It can issue identity |
|---|---|---|
| `--admin-token` / `ZETTA_ADMIN_TOKEN` | the root **derived** from the token: Ed25519 seeded by `SHA-256("zetta-identity-root-v1\n" + token)`; its seed is stored (file mode 0600), so the root outlives a token rotation | yes |
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

## Rotating the root (#1342)

The pinned root does not move by itself: a node restarted with a new `ZETTA_ADMIN_TOKEN` keeps the
root it pinned. Moving the cluster to a new root is a deliberate step, a **succession**.

**Decided under Steve's 2026-11-14 delegation:** rotation does not revoke the tokens issued under the
old root by default, with an optional `revoke_tokens: true` for a rotation after a compromise,
because an ordinary rotation (a token that reached a log, a staff change) should not lock every user
and app out, while a compromise needs every credential gone.

1. **Run it on a node that holds the current root.** `rotate_identity_root {new_admin_token,
   revoke_tokens?}` (MCP and REST, needs `cluster:admin`, audited per call;
   `Node::rotate_identity_root`) refuses on a verify-only node. It derives the new root from the new
   admin token.
2. **The succession record.** `identity_root_succession:<new root's public key>` holds `{"old",
   "new", "issued_at", "_seal"}`. The seal is `"ed25519-succession-v1:<128 hex>"`, the **old** root's
   signature over `zetta-identity-succession-v1`, old, new and `issued_at`. Its own prefix keeps it
   from passing for a record seal. The key names the new root, so the record cannot be moved to hand
   identity to another one, and it never changes, so it carries no version.
3. **Re-sealing.** The rotating node moves to the new root first, then re-seals every identity record
   it holds under it at a newer version of each key (`replicate_identity`); a removal stays a sealed
   removal. With `revoke_tokens`, every `token:` record is removed instead. The owner (the user named
   `owner`) is issued a token whose secret is the new admin token, under the id the new root fixes, so
   a node restarted with it authenticates the owner as before. The pin is rewritten with the new
   root's seed.
4. **Every other node moves its pin.** `Node::follow_identity_succession` runs before identity is
   reconciled, on every full reconcile and whenever a succession key changes. It follows the chain of
   verified successions from the root the node pinned (`identity_succession_chain`; where one root has
   two successors, the one issued last wins, then the larger key) and takes the last root. When that
   root is the one the node's own admin token derives (`NodeConfig::identity_owner_candidate`), the
   node issues identity again; otherwise it verifies only. It rewrites `identity-root.json`
   atomically (`repin_identity_root`) with the roots it left in `previous` (oldest first, at most
   eight), then reconciles every identity family again from the whole document, since records the new
   root sealed may have arrived before the succession and been refused.
5. **The old root is history.** After the move a node opens a record under the new root only. A record
   the old root sealed stays honored only while it is **exactly** the record the node already held for
   that key, until the re-sealed version (newer) replaces it; it is not written back as a replay. A
   record the old root seals after the move does not open, so a leaked old seed issues nothing.
   `identity-held.json` is re-opened on load under the current root or one of the `previous` roots.
6. **A node restarted with the new token.** Its pin still names the old root, so it starts as before
   (verify-only, unless it stored the old seed) with the token's root as the candidate. It issues
   identity only once the document gives it a chain of successions to that root. Until then the
   warning says: start one node with the previous admin token and run `rotate_identity_root`.
7. **Joiners.** `--identity-root` may name the current root or an older one; a joiner pinned to an
   older root follows the chain the same way.

A root the cluster already left cannot be the target of a rotation: the chain would become a cycle that
nodes pinned at different points read differently. **Run one rotation at a time.** Two nodes rotating from
the same root at once both write a succession: every node still on the old root follows the one issued
last, but the node that made the other has already moved to its own new root and stays there, and the
two sides honor none of each other's new records. Rotate that node again, from the root it is on, to the
cluster's.

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
- **A node that never saw a change** refuses an older sealed record once the epoch that commits the
  newer version reaches it. A brand-new cluster offered an old epoch with nothing newer left anywhere
  needs `--identity-min-epoch` ([the identity epoch](#the-identity-epoch-1360)).
- **Rotating the owner token keeps the root, on nodes that already hold it.** The root's seed is
  stored in `identity-root.json` (mode 0600) the first time it is derived, so a node restarted with a
  new `ZETTA_ADMIN_TOKEN` keeps issuing identity under the same root. A node **new to the cluster
  after the token changed** derives a different root from the new token and joins verify-only: handing
  it the root's seed is not built yet ([zetta#1479](https://github.com/oramasearch/zetta/issues/1479)).
  Moving the cluster to the new token's root is a rotation ([Rotating the root](#rotating-the-root-1342)).
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
  `seal_preexisting_identity`, `seeded_identity_id`, `bootstrap_owner`, and the rotation:
  `rotate_identity_root`, `follow_identity_succession`, `open_held_identity` (#1342), and the epoch:
  `accept_identity_epoch`, `maybe_write_identity_epoch`, `committed_identity_versions` (#1360).
- `src/core/api/identity_capabilities.cpp` — the `rotate_identity_root` and `get_identity_epoch`
  capabilities.
- `src/core/governance/principal.cpp` — sealed access keys and their sealed removal.
- `src/main.cpp` — root setup, seeding after the join, the join command with `--identity-root`.
- Tests: `tests/governance/test_identity_seal.cpp` (seal, versions, tombstones, pin, node
  behaviour, the held records across a restart, upgrade),
  `tests/governance/test_identity_rotation.cpp` (successions, the chain, two nodes rotating, the
  old root refused after the move, a forged succession, a restart with the new token, #1342),
  `tests/governance/test_identity_epoch.cpp` (the epoch's digest and growth, a fresh node refusing a
  pre-revocation record, an older or forged epoch refused, the minimum epoch, #1360),
  `tests/cluster/test_cluster_join_authority.cpp` (two nodes over mutual TLS: #848 steps 4 and 5, and
  #1341: a member replaying a revoked token and a removed assignment, and deleting identity with
  unsealed removals),
  `tests/governance/test_authz_regressions.cpp` (`AuthzCluster.Finding763_*`, `Finding764_*`).
