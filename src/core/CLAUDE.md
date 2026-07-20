# src/core — zetta-core implementation rules

The static core library. Everything here must run **headless** — no SDL/ImGui
type may appear in this tree, ever. **Namespace == directory** (`src/core/<d>/`
implements `namespace zetta::<d>`, mirroring `include/zetta/<d>/`; a class's
nested/pImpl declarations stay nested). Orient with
[docs/architecture/code-map.md](../../docs/architecture/code-map.md); read the
touched domain's `docs/<domain>/README.md` before changing its code.

## Replication (the load-bearing invariants)

- **The CRDT document is the source of truth**; the on-disk store + in-memory
  tree are its projection. Local mutations are **captured** at the data-model
  layer (`Node`/`Database`/`Table`/stores hold a `ReplicationManager*`) so every
  write path replicates; `Node::tick()` **reconciles** the projection from the
  document — **materialize-only, never re-capture** (no feedback loop) — then
  persists.
- **Reconcile/`materialize_*`/`apply_value` paths never emit audit events** — a
  peer's action arrives already-audited as its own replicated `audit:` key.
- **Removal is a tombstone, never a key delete.** `ReplicationManager::remove`
  writes `{"_removed": true}`; `snapshot()` omits tombstoned keys (so reconcile
  still reads a removal as plain absence) and removal **wins** over a concurrent
  write. Never reintroduce a CRDT map delete: it names only the predecessor
  writes the deleting node had already merged, so an unseen write resurrects the
  key on every node including the remover's (#838). A caller that must see
  through the tombstone — `migrate_to_replication`, which would otherwise
  re-capture a dropped entity from a stale disk projection — asks `knows_key()`.
  `row:` keeps its own `{tbl, deleted:true}` shape, because reconcile has to see
  that entry to route the delete to a table.
- **Flat CRDT keys, one entity per UUID key** (`src/core/catalog.hpp` is the
  internal scheme): `node:` `db:` `tbl:` `row:` `lo:` `project:` `input:`
  `graph:` `deployment:` `run:` `model:` `job:` `index:` `principal:` `key:`
  `audit:` `part:`. New shared state = a new key/shape, replicated by the
  existing gossip — never a parallel hand-synced copy. Bodies (large objects,
  model weights, input payloads) stay **off** the CRDT: on disk, transferred
  peer-to-peer, verified against size + sha256.
- **Thread safety:** the cluster's network thread may only touch thread-safe
  things (`ReplicationManager` is mutex-guarded; `BlobStore` works off the CRDT
  + disk). The `Node` tree / stores are **main-thread only**.

## Boundaries and errors

- **No exceptions across zetta-core seams** (the lib builds with
  `-fexperimental-library`; nothing may unwind across it). Boundary functions
  are non-throwing: `std::optional` / status types + a path-qualified reason.
  `runtime::parallel_for` bodies must not throw (a throwing body aborts).
- **Validate untrusted input at the boundary.** Top-level replicated entities
  (Run, Model, ComputeJob, AuditEvent, Index, DatasetPartition, …) use the
  embedded draft-07 schema + the pboettch validator (the `parse_run_value`
  pattern). Value-type DTOs (configs, specs, policies) use the hand-written
  non-throwing `from_json` mirroring their type's `*_schema()` accessor
  (registered in `zetta::schemas::boundary_schemas()`). Binary formats
  (Parquet, Snappy, checkpoints) are bounds-checked, never trusted.
- **Schema as code:** the C++ embedded schema (a `*_schema()` accessor) is the
  source of truth; `schemas/*.json` is generated + drift-checked. See
  [docs/architecture/schemas.md](../../docs/architecture/schemas.md).

## Seams (extend, don't special-case)

New capability = a new implementation of an existing seam: `StorageEngine`,
`Dialect` (the `Executor` stays dialect-neutral), `StepExecutor`, `Check`,
`ComputeDevice`, `IndexBackend`, `Partitioner`, `Provisioner` +
`<Vendor>ControlPlane` (CLI via the shared `CommandRunner`, no cloud SDK),
`LanguageFrontend`, `ReplicationPolicy`. Injection over hard dependencies
(`FleetSource`, `IdentityHooks`, `NarratorCompletion`, `DatasetResolver`) keeps
zetta-core free of HTTP/LLM/cloud SDKs and unit-testable against fakes.

- Exactly two TUs name Corosio (via `zetta::runtime::net`):
  `src/core/cluster/cluster.cpp` for inbound/peer traffic — including the TLS
  surface, #466 — and `src/core/api/native_http_transport.cpp` for outbound
  client traffic (the native coroutine HTTPS client behind
  `connect::HttpTransport`, #827; it lives in the `zetta-api` target because it
  also needs Boost.HTTP, which zetta-core must never link).
  OpenSSL libcrypto is named by exactly two
  TUs — `src/core/cluster/security.cpp` (join-token/CA/cert minting) and
  `src/core/governance/token_secret.cpp` (token-secret RAND/SHA-256/constant-time
  compare, #744); only `src/core/cluster/replication.cpp` names automerge-cpp.
  Everything else goes through `zetta::runtime` / the public seams — never
  `boost::capy`/`boost::corosio`/`openssl/*`.
- Observable state defaults to a virtual read-only `system.<table>` computed at
  query time from the live source (RULE-7); plain names, no `zetta_` prefix for
  new tables. Ephemeral/observed state (telemetry, cloud inspects, checkpoints)
  is a read view with **no** CRDT backing.

Add or extend tests in `tests/` for any core-logic change, and update the
touched `docs/<domain>/` page + this network (see root `CLAUDE.md` →
Documentation) in the same commit.
