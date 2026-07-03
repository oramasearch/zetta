# Activity feed — one chronological view of everything the cluster was asked to do

The Operations → Activity view (#622; epic companions #575/#614): a single
read-only timeline answering **"what happened in the last hour and who asked
for it"**. For the drive-programmatically / watch-visually split, this is the
first thing an operator looks at: runs, deploys, compute jobs, key changes,
provision requests, and every MCP call land in ONE feed, each entry with its
actor, **surface** (GUI · console · MCP), action, target, outcome, and time,
with click-through to the owning panel.

## Composition — existing sources only (RULE-7)

`zetta::observability::activity_entries(const Node&)`
(`include/zetta/observability/activity.hpp`) composes the feed **at call
time** from two sources, newest first (fixed-width ISO-8601 stamps sort as
plain text):

- **The audit trail** (`Node::audit_events()`) — one entry per event. The
  trail already records graph changes, deploys (`graph_deployed`), compute
  jobs (`job_created` / `job_finished`), key issuance/revocation, provision
  requests, and every capability call (`data_access` on `api:*` targets with
  the acting key), so jobs, deployments, keys, and API calls enter the feed
  through their audit events — which is also where the **actor** lives.
- **Runs** (`Node::runs()`) — the one source with no audit coverage. Each run
  contributes a "run created" entry at its `created` stamp and, once
  terminal, a "run succeeded" / "run failed" entry at its `updated` stamp
  (approximate finish time; the terminal `set_status` is normally the last
  mutation). A failed run's entry carries the run's last error log line
  verbatim.

There is **no new stored state and no new JSON format**: every call recomputes
the merge from already-validated in-memory entities, so the feed can never
drift from its sources.

### Surface derivation — never stored, derived per entry

`ActivitySurface` is derived from fields the sources already carry:

| Surface | Derivation | Rendered as |
|---|---|---|
| `api` | audit target starts with `api:` (the capability registry's shape, #615) | **MCP** (REST gets its own label when the REST rendering #618 lands) |
| `console` | audit detail starts with `console ` (the governed console's shape) | console |
| `local` | everything else (in-process actions) | **GUI** in the headed app |

This is what makes an MCP-driven action and a GUI-driven action visually
distinguishable: external surfaces render in primary ink, local ones quiet.

`ActivityOutcome` maps allow/deny/status: `ok`, `denied` (governance refused —
a data-access deny), `failed` (the engine attempted and failed — a blocked
deploy, a failed job or run), `info` (created/queued, no outcome yet). The
tone mapping is one `tone_of(ActivityOutcome)` overload in `src/gui/tones.hpp`
(#636). `ref_kind`/`ref_id` name the owning entity ("run" / "job" / "graph" /
"deployment" / "audit") so renderers deep-link without re-deriving it.

## system.activity — the queryable twin (RULE-7)

The same function serves `SELECT * FROM system.activity` (executor.cpp):
columns `time · surface · actor · action · target · outcome · detail ·
ref_kind · ref_id`, newest first — an agent reads the exact feed the operator
watches. See [../data/query.md](../data/query.md).

## The view (src/gui/app_activity.cpp)

One registry row (Operations, leading the section), drawn from the grammar:

- `page_header` + two filters: a **time window** (last hour · last 24 hours ·
  all time; the cutoff is `util::iso8601_seconds_ago` compared
  lexicographically) and a **surface** filter (all · GUI · console · MCP).
  The filters are the panel's only state — the feed is recomputed each frame.
- The feed table: Time · Surface · Actor · Action · Target · Outcome ·
  Detail, in Geist Mono. The **target is an `entity_link`**: runs open
  Runs (selected), jobs open Compute jobs (selected), graph changes open the
  Graph workspace (the named graph opened, #633), deploys open Deploy (the
  deployment row highlighted, #633), everything else opens Admin & audit.
  Outcome renders as a tone-coloured word.
- `empty_state` when the node has no activity at all; a muted "widen the
  window" line when the filters hide everything.

**Home strip**: Home renders the newest five entries as a quiet mono strip
(`RECENT ACTIVITY`: dot · clock · surface · action · target) with a "View all
activity" link into the full view.

## Boundaries / later slices

- **No new JSON format** — the feed is computed; nothing is stored or parsed.
- **Dataset adds** are not in the feed yet: adding a dataset records no
  audited, timestamped action today (partition `created` stamps exist but
  carry no actor). They join when dataset adds are audited.
- **Run provenance**: a run records no actor/surface of its own; an
  MCP-driven run today appears as the MCP-surfaced capability call adjacent
  to the run's own entries. When the run-submitting capabilities land
  (#616/#617), their audited `api:` calls are the MCP-surfaced record.
- **REST** renders under the `api` surface until #618 gives the server layer
  a way to tell the renderings apart.
- The **activity hook** (#620) marking panels dirty / deep-linking on an
  external mutation is a separate slice; this view polls the live sources
  each frame like every other panel.
