# Secret references (CTL-3)

> **Control:** credentials are stored as references resolved from a store, and are never
> embedded in project or run records, read surfaces or logs
> ([#74](https://github.com/oramasearch/zetta/issues/74), `CTL-3`; it carries the closed
> `RULE-6` [#214](https://github.com/oramasearch/zetta/issues/214) and mitigates `THR-3`
> [#258](https://github.com/oramasearch/zetta/issues/258)).

This page is the one account of where a credential that zetta must **replay** to an external
system (a Snowflake token, a Postgres password, an action connector's bearer token) lives, how it
is named, and every surface it must never reach. zetta's **own** credentials (tokens, access keys)
are a different problem: they only need verifying, so they are stored hashed
([access-control.md](access-control.md), `governance/token_secret.hpp`). A replayed credential
cannot be hashed; it has to be recoverable by the node that presents it.

## The store and the reference: saved connections

A source credential is saved once, **by name**, in the saved-connection store
(`connect/connection_store.hpp`, [#827](https://github.com/oramasearch/zetta/issues/827); the
full page is [connector query → saved connections](../data/connector-query.md#saved-connections)).

| Requirement | How it is met | Proof |
|---|---|---|
| **A record names the credential; the store resolves it.** | `create_connection {name, connector}` stores it; `create_dataset_from_query` and `list_source_tables` take `connection: "<name>"` and `find_connection` resolves the bytes in memory for the query that presents them. A co-launched distributed fetch carries only the name: each node resolves the credential from its own copy, so no credential travels in the launch (`connect/distributed_ingest.hpp`). | `tests/connect/test_connection_store.cpp` `SavesAndResolvesAConnectionWithItsToken` |
| **Replayable, not hashed.** | The credential is **sealed**: AES-256-GCM under a key derived (HKDF-SHA256) from a root secret held outside the replicated catalogue, bound to the connection name as associated data (`connect/credential_seal.hpp`, [#1230](https://github.com/oramasearch/zetta/issues/1230)). | `tests/connect/test_credential_seal.cpp` (fresh nonces, wrong key, wrong connection, tampering) |
| **Not plaintext at rest.** | The replicated row holds the settings without the token plus the sealed value; a legacy plaintext row is re-sealed on start; a node with no sealing key refuses to save (fail closed). | `ConnectionStoreSealed.TheStoredRowAndTheCatalogueHoldOnlyTheSealedCredential`, `ALegacyPlaintextRowIsResealedOnStart` |
| **The root secret is outside `data_dir` (#1314).** | A node started by `main` reads it from `--connection-root-secret-file PATH` / `ZETTA_CONNECTION_ROOT_SECRET_FILE`, a file outside the data directory such as a mounted Kubernetes Secret, held in memory only. A file that resolves inside `data_dir` (links followed), is writable by group or others, or holds a secret shorter than 32 characters is refused and the node does not start. Without the file the node holds **no keyring**: `create_connection` is refused and saved connections are `unreadable`; every other capability runs, and start logs why. It never falls back to a secret kept in `data_dir`. | `ConnectionStoreSealed.ACopiedDataDirectoryDoesNotOpenWithoutTheExternalSecret`, `CredentialSeal.TheExternalRootSecretMustLiveOutsideTheDataDirectory` |
| **Rotation.** | Saving under the same name seals the new credential afresh. The secret file's first line is the current secret and later lines are retired ones that still open; each node re-seals the store at its next start. | `RotationResealsUnderTheNewRootSecret`, `SavingANewCredentialReseals` |
| **Migration from the join-token key.** | Before #1314 the key derived from the cluster join token, persisted in `<data_dir>/security/cluster-secret.json`. With the file supplied, `main` keeps the join tokens as retired secrets, so the first start re-seals every row under the external secret; after that the join token opens nothing. Roll every member together. The operator wiring is [orama-operator#1166](https://github.com/oramasearch/orama-operator/issues/1166). | `ConnectionStoreSealed.RowsSealedUnderTheJoinTokenMigrateToTheExternalSecret` |
| **No read surface carries it.** | `system.connections`, `list_connections`, `create_connection`'s output and the web form render through `SavedConnection::to_redacted_json()`, which says only `credential: set / none / unreadable / plaintext`. SQL cannot reach the store's database. | `NoReadSurfaceEverCarriesTheToken`, `ReadSurfacesSayOnlyThatACredentialIsSet`, the in-app `cluster.connections` case |
| **No log line carries it.** | `HttpRequest::secret_headers` names the headers whose values never reach a reason string, log line or span (`connect/http_transport.hpp`). | `tests/connect/` transport tests |

## A credential given inline is used, never kept or echoed

`create_dataset_from_query` and `list_source_tables` also accept an inline `connector` (with its
`token`) for a one-off, and the action connectors (`enqueue_action_task`, `deliver_action_task`,
`publish_report`) take a `token`. Those values are used in memory and are **not** written to any
record:

- the action task record stores the endpoint and kind, never the token; `deliver_action_task`
  takes the token again on each delivery (`solutions/action_integration.cpp`, `to_json(ActionTask)`);
- an inline connector never co-launches a distributed fetch (peers hold no credential), so it never
  rides a launch.

**The consent challenge** is the one place an inline credential was rendered back out, and is
closed by #74. A consent-gated call (a remote source, a cloud provisioner) freezes its validated
input on the node's consent ledger so approval executes exactly what was asked. That input was
returned verbatim in the `consent_required` refusal's `error.details` and listed by
`list_consent_requests` — to a **different** principal, the approver. Now
`governance::to_json(ConsentChallenge)` renders the input (and the executed result) through
`governance::redact_credentials`, which replaces the value of every credential field with
`"[redacted]"`. The ledger itself still holds the real input in memory, so approval runs the
frozen request unchanged; the ledger is never persisted or replicated.

Which fields are credentials is one rule, `governance::is_credential_field`
(`include/zetta/governance/credential_redaction.hpp`): `token`, `secret`, `password`, `api_key`,
and any name ending in `_token`, `_secret` or `_password` (`delivery_token`). The web form's masked
inputs use the same rule. A new capability that takes a credential names its field by that rule,
and is then redacted on every surface that renders input.

Proof: `tests/governance/test_consent.cpp` `AnInlineCredentialIsHeldToExecuteButNeverRendered`,
`CredentialRedaction.NamesTheCredentialFieldsAndOnlyThose`; `tests/api/test_consent.cpp`
`AnInlineConnectorTokenNeverAppearsInTheChallenge` (the refusal, `list_consent_requests` and
`system.consent_requests`).

## What is not met yet

- **An embedded node keeps its root secret beside the data.** A library or test node built
  without `connection_root_secret_required` creates a node-local secret in
  `<data_dir>/security/connection-root-secret`, which a copy of `data_dir` carries. `main` never
  does this since [#1314](https://github.com/oramasearch/zetta/issues/1314); an embedding that
  stores real credentials must pass a secret it holds elsewhere.
- **`DeploymentTarget::credentials_ref`** (`pipeline/deployment_target.hpp`) is declared as a
  reference but nothing resolves it: only `Local` targets are functional and they take no
  credential. When a remote placement kind is built, its `credentials_ref` resolves through the
  saved-connection store: [#1315](https://github.com/oramasearch/zetta/issues/1315).
