# Scoped app access to a workspace Zetta

Issue [#1280](https://github.com/oramasearch/zetta/issues/1280) gives each
declared app connection one revocable bearer credential for the workspace's
Zetta. The credential is enforced by Zetta's existing user, role, token,
object-grant, capability, and audit machinery.

The Lake control plane supplies the `credential_scope` selected for a
connection. Zetta turns that scope into a connection-owned user and
least-privilege role, returns the plaintext token once, and stores only the
token's salted hash. The operator remains responsible for projecting the
one-time secret to the app and reconciling the connection lifecycle.

## Issue and revoke

An administrator with `api:manage_tokens` (`tokens:manage`) calls
`issue_app_credential`:

```json
{
  "connection_id": "conn_01",
  "credential_scope": {
    "access": "read",
    "schema": "app",
    "tables": ["orders"],
    "topics": ["orders.events."]
  }
}
```

At least one of `schema`, `tables`, or `topics` is required. A table may be
qualified as `<schema>.<table>` or may inherit `credential_scope.schema`. A
qualified table must agree with that schema. Supplying a schema without tables
grants the whole database; supplying tables grants only those tables. Topic
values are name prefixes and may optionally start with `topic:`.

The answer includes `user_id`, `role_id`, `token_id`, the normalized scope, and
`credential`. The plaintext `credential` appears only in this answer. A second
live credential for the same connection is a `conflict`; revoke the current
credential before issuing its replacement. Lake's connection row identifies
the source app, so no second app identifier participates in credential identity.

`revoke_app_credential` takes `connection_id`. It revokes every token issued to
that connection-owned user and removes its role assignment.
Repeating the call succeeds with `revoked: 0`, which lets an operator reconcile
desired absence safely.

## Enforcement

`read` grants:

- `sql:read` on only the named database or tables and access to only the `sql`
  tool when SQL resources are present;
- `topics:read` and `pubsub:subscribe` on only the named topic prefixes, plus
  the read and receive topic tools when topic resources are present.

`write` includes those read permissions and adds `sql:write` for the same
database or tables plus publish, update, and delete-event rights for the same
topic prefixes. It does not grant DDL, user or token administration, topic
creation or deletion, network-record access, or unrelated tools.

Every REST or MCP call still passes through the capability registry. SQL checks
the acting user again for each statement and each table. Pub/sub checks the
acting user against the requested topic prefix. Registry calls and SQL data
changes use the existing immutable audit trail. Revocation immediately makes
the bearer fail authentication; removal of the role also makes an already
resolved identity fail authorization.

The connection identity and role are implementation-owned. If either is found
with an unrelated role or user assignment, issuance returns `conflict` instead
of broadening or replacing permissions it does not own.

## Boundaries and tests

The two capabilities publish their input and output schemas through the
registry, OpenAPI, and the spec index. No new stored JSON format is introduced:
the durable state is the existing replicated `User`, `Role`, `UserRole`, and
hashed `Token` entities.

`tests/api/test_network_capabilities.cpp` proves that an unrelated caller
cannot issue a credential, a read credential can select its granted app table
but cannot write it or read another app's table, topic prefixes are isolated,
each use is audited, duplicate live issuance is refused, and revocation blocks
both authentication and authorization.

