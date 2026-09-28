# Governance docs

Governed self-serve: **deny-by-default, key-scoped access** (a request inherits the scope +
capabilities of the key it logged in with, RULE-3) and an **immutable, append-only audit trail** for
every governed action — the two controls every surface (deploy, run, console, cloud burst) enforces
through. Read here when adding any action a user can take: it must be capability-checked at the boundary
and recorded as an `AuditEvent`. The read-only GUI is [../gui/admin-panel.md](../gui/admin-panel.md);
the gate that governs *correctness* (vs access) is [../pipeline/gate.md](../pipeline/gate.md).

- [access-control.md](access-control.md) — key-scoped access (V1): `Principal` + `AccessKey` + `Scope` (resources) + `Capability` (rights, TTL'd), deny-by-default (RULE-3 / ENT-22). Plus the **credential store** (#744): `User` + `Role` + `Token` (hashed at rest) + `UserRole`, and the bootstrap owner (#733); the **default-deny gate** (#745, always on since #799 — no auth flag/toggle; `--require-auth` and `ZETTA_REQUIRE_AUTH` were removed); and **RBAC roles** (#746): the permission vocabulary, built-in admin/writer/reader reconciled on bootstrap, custom roles + user↔role assignment via SQL (gated on `roles:manage`), surfaced in `system.roles` / `system.user_roles`.
- [audit-trail.md](audit-trail.md) — the immutable, append-only `AuditEvent`: graph change / deploy / data-access / decision-policy events, replicated + JSON-Schema validated (ENT-23 / FEAT-9).
- [../pubsub/crud.md](../pubsub/crud.md) — separate topic/event rights and per-sequence,
  hash-only mutation audit records for node-local pub/sub CRUD (#1136).
- [governed-console.md](governed-console.md) — the governed Console/REPL screen: every statement capability-scoped, sim-Python gated before it runs, each statement audited (SCR-8 / #525).
- Remote consent for cloud-touching actions (#621) lives with its surface: [../api/consent.md](../api/consent.md) — `ConsentLedger` (governance) + the challenge/approval protocol (api); approvals must come from a different principal, audited on both sides. **Enforcement is OFF by default (#995)** — a gated capability executes on the first call and the ungated call audits as `consent_bypassed`; `--governance` turns the challenge back on.
