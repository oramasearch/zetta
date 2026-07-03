# Admin & Settings panel

The headed app's **governance surface** (`App::draw_admin`, `src/gui/app.cpp`) —
the GUI slice of **FEAT-9**, realizing screen **SCR-7** / component **CMP-5**. It
surfaces the three governance entities zetta already models so an operator can
*see* who can do what, and what has been done, without dropping to the C++ API or
SQL:

- **principals + their access keys** — the authenticated actors and the
  credentials they log in with ([access-control.md](../governance/access-control.md));
- **scope + capability grants** — per key, *which resources* it reaches and
  *which rights* it may exercise (deny-by-default, the capabilities TTL'd);
- **the audit log** — the immutable, append-only trail of every governed action
  ([audit-trail.md](../governance/audit-trail.md)).

Like the [Runs panel](../pipeline/runs.md#gui-panel), it is a **read-only viewer**. It renders
whatever governance state the node holds (created via the API / tests, or
replicated from a peer), so a fresh standalone node simply shows "(no principals)"
and "(no audit events)" until something creates them. **Mutation** — issuing a
key, granting a capability as a first-class command/event (ENT-22 →
`GrantCapability` / `CapabilityGranted`), or approval gates — is a **later FEAT-9
slice**; this slice is the "view principals/keys, grants, and audit events"
acceptance only.

## Layout

The panel is one collapsing header, **Admin & Settings**, with two sections.

### Principals & access keys

Rendered through the [master-detail scaffold](master-detail.md) (#634), at a
fixed height so the audit log keeps its place below:

- **Principal list** (left) — one row per `node_.principal_names()` entry: a
  standard `MasterRow` with a muted dot, the principal's name as the title,
  and the kind (`user` / `service` / `node`) as the mono meta.
  Selecting a row sets the active principal (held by name and re-resolved each
  frame via `node_.find_principal`, like the rest of the UI, so a dropped
  principal can't leave a dangling handle).
- **Detail** (right) — for the selected principal: its name, kind, id, and
  `created` stamp, then **each of its access keys**. Per key
  (`render_access_key`):
  - the key's short id + `created` stamp;
  - **scope** — the resources the key may touch (the resource axis, RULE-3):
    `unrestricted (admin)` (amber), `(no resources …)` when empty, or a bulleted
    `kind: name` list of grants;
  - **capabilities** — the rights the key may exercise (the verb axis, ENT-22):
    `(none — denies every operation)` when empty, or a bulleted `domain:action`
    list. A TTL'd grant shows its `expires` stamp; once elapsed it is flagged
    **red** as `(expired …)` (compared against `zetta::util::now_iso8601()`).

This single view covers both halves of the acceptance — "principals/keys" and
"grants" — because a key's two grant axes (scope + capabilities) are exactly what
the detail pane renders.

### Audit log

A scrolling, header-pinned table over `node_.audit_events()` (`render_audit_log`),
in chronological order (matching the API's contract), with one row per event:

| Column | Source | Notes |
|---|---|---|
| Time | `event.time()` | ISO-8601 UTC stamp. |
| Action | `event.action()` | `graph_created` / `graph_dropped` / `graph_deployed` / `data_access`. |
| Actor | `event.actor()` | The login key id / principal, or `—`. |
| Target | `event.target()` | The graph name / deployment id / resource, or `—`. |
| Outcome | `event.allowed()` | `allow` (green) or `deny` (red). |
| Detail | `event.detail()` | Human-readable reason / summary, or `—`. |

## Boundary validation

The panel reads **already-validated** in-memory handles — `Principal`,
`AccessKey` (+ its `Scope` / `CapabilitySet`), and `AuditEvent` — every one of
which passed its JSON Schema at the boundary when it entered from disk or the CRDT
(`scope_from_json` / `capability_set_from_json` for keys; `parse_audit_event_value`
for events). So this slice introduces **no new JSON format and no new boundary**,
the same way the [Runs panel](../pipeline/runs.md#gui-panel) reuses `parse_run_value`.

## Where it fits

This is the **view** slice of FEAT-9's GUI. It sits alongside the existing read-only
panels (Cluster, Runs) and depends on three shipped subsystems:
[key-scoped access](../governance/access-control.md), the [capability model](../governance/access-control.md#the-capability-decision-ent-22),
and the [audit trail](../governance/audit-trail.md). The natural next slices are the **mutating**
admin actions (issue a key, grant/revoke a capability, approval gates) and a
queryable **`system.zetta_audit`** SQL table.

## Pointers

- `src/gui/app.cpp` — `App::draw_admin` + the file-local `render_access_key` /
  `render_principal_detail` / `render_audit_log` view helpers.
- [access-control.md](../governance/access-control.md) — the `Principal` + `AccessKey` + `Scope`
  + `Capability` model the principals/keys/grants section renders.
- [audit-trail.md](../governance/audit-trail.md) — the immutable `AuditEvent` trail the audit-log
  table renders.
- [runs.md](../pipeline/runs.md#gui-panel) — the sibling read-only viewer panel this mirrors.
