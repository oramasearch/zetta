# Product-language copy audit (#635)

Every view audited against the product vocabulary (projects, pipelines,
deploys, runs, models, datasets, nodes — [../product/orama-zetta.md](../product/orama-zetta.md))
and the design system's copy rules ([design-system.md](design-system.md)):
sentence case, no em-dashes, no engine-speak as primary text. This page is the
sign-off checklist the issue's acceptance requires, plus the rules that keep
it true.

## The rules (enforced, not advisory)

1. **No raw enum / internal kind string as primary text.** A kind or status
   enum renders through `label_of(...)` in `src/gui/labels.hpp` — the ONE
   place an engine enum maps to product words (the `tone_of` pattern: one
   overload per enum, never a panel-local switch). Compile-time
   `static_assert`s lock the load-bearing labels (`ddf` → "Data transform",
   `featurize` → "Feature build", `near_data` → "Near data").
2. **SQL keeps raw values; panels render words.** The canonical lowercase wire
   names ("featurize", "queued", "data_flow") remain exactly what SQL, the
   `system.*` views, the JSON schemas, and the API return. Only the GUI's
   rendering changes.
3. **Free-text kind tokens go through `humanize_kind(raw)`** (also
   `labels.hpp`): known tokens get their product words; unknown tokens degrade
   gracefully (underscores → spaces, first letter capitalized) so a new wire
   name is never rendered raw even before it gets a proper mapping.
4. **Technical detail demotes to the mono metadata strip.** A raw wire value
   may appear in a labelled `mono_line` (e.g. `kind: featurize` under a human
   headline) — that is the sanctioned place, never a row title, column header,
   headline, button, or banner.
5. **Where a concept must stay technical** (partition, coordinator, near
   data), the label is the readable form and `term_hint("…")`
   (app_internal.hpp) names what it means on hover — one plain sentence, no
   jargon in the explanation itself.
6. **Empty-cell placeholder is `·`**, never an em-dash (the design system's
   no-em-dash rule applies to every visible string, placeholders included).
7. **Engine/infra vocabulary stays out of user copy**: peer → node · large
   object → file · replica/replication (as user copy) → sync/copy · unlink →
   remove · wire → network · signal plane / compute plane / coroutine /
   provisioner / internal symbols (`ScalingPolicy::evaluate()`), rule ids
   (RULE-1), release labels (R1) → say what it means instead.

## Per-view sign-off

Audited file by file (every visible string literal + every enum render site),
fixed where offending, and re-checked. "Clean" = conforms to the rules above.

| View / TU | Verdict |
|---|---|
| Home + About (`app_home.cpp`) | Fixed: em-dash placeholder tile, run status via `label_of`, tagline casing matched to the brand phrase. Clean. |
| Explorer + Console shell (`app_explorer.cpp`) | Fixed: "Large objects" → "Files", body/peer/unlink vocabulary, "drop project" → "delete project", "execution-graph IR" → "execution graph", Postgres casing. Clean. |
| Search (`app_search.cpp`) | Fixed: raw `SearchPath` enum → "Whole cluster"/"Single node" + `term_hint`, "peer" → "other node", "Wire bytes" → "Bytes moved", `create_index` dropped from copy. Clean. |
| Command palette (`app_palette.cpp`) | Fixed: success banner uses the capability's title, "no action named". Raw ids/schema field names remain in the mono/code slots (a power surface; sanctioned). Clean. |
| Cluster (`app_cluster.cpp`) | Fixed: REPLICATION → SYNC section, replica/peer/signal-plane/compute-plane/coroutine/provisioner copy, "over the wire" legend, Raft `role` line dropped (contradicted "all nodes are equal"), flow captions via `humanize_kind`, em-dash placeholder. Clean. |
| Cloud inspector (`app_cloud.cpp`) | Clean on audit; no changes needed. |
| Governance / Admin (`app_governance.cpp`) | Fixed: audit Action column via `humanize_kind` (was raw `graph_deployed`-style tokens), "replication from a peer" empty state, em-dash placeholders, "Key" casing. Scope/capability identifiers (`database:`, `api:read`, `[user]`) stay as mono metadata. Clean. |
| Activity (`app_activity.cpp`) | Fixed: em-dash placeholders. Action strings were already humanized. Clean. |
| Traces (`app_traces.cpp`) | Fixed: sentence-case subtitle + hint. Span names render as recorded (fixing engine-speak span names belongs at the span-naming source, not the renderer). Clean. |
| Pipeline workspace (`app_graph_workspace.cpp`) | Fixed: step-kind combo/stage column/pickers via `label_of`, edge kinds "Data flow"/"Control" (wire token gone), em-dashes. Canvas stage eyebrows route through `label_of`. Clean. |
| Deploy + Gate (`app_deploy.cpp`) | Fixed: check status via `label_of` (ALL-CAPS badge removed), `DeploymentKind` labels ("VPC", not "Vpc") in dial/banner/table, RULE-1/R1 leaks, em-dash placeholder. Clean. |
| Runs (`app_runs.cpp`) | Fixed: job kind/status via `label_of`, run status labels, em-dash placeholders, "deployed graph" → "deployed pipeline" in prose. Output kinds ("dataset"/"model"/"index") are product words in a mono column; sanctioned. Clean. |
| Compute jobs (`app_jobs.cpp`) | Fixed: row meta + detail via `label_of` (raw `ddf` gone), scaling objective/action combos, `ScalingPolicy::evaluate()` symbol removed from copy. The raw wire kind stays as `kind:` in the mono strip under the human label. Clean. |
| Models (`app_models.cpp`) | Fixed: `system.models` table name in copy, "Model" headline casing, "Clear" button, "(awaiting body transfer)" → "(syncing)". Free-text model kind ("classifier") is user data; sanctioned. Clean. |
| Datasets (`app_datasets.cpp`) | Fixed: progress phrases sentence-cased, em-dash. Replication strategy token (`replicate-to-all`) stays as labelled mono metadata, explained in plain copy below. Clean. |
| Test suite (`app_test_suite.cpp`) | Fixed: outcome via `label_of` (ALL-CAPS outside pill removed), "loopback cluster"/"off the UI thread" copy, "Running…" casing. Category titles are core `category_title` product words ("DDF dataframe ops" names the subsystem on a practitioner surface; accepted). Clean. |
| Console component (`repl_console.cpp`) | Fixed: em-dash cell placeholder, "frontend" caption → "Language". Clean. |
| View registry / sidebar (`app_views.cpp`) | Clean on audit; all names sentence-case product vocabulary. |
| App shell (`app.cpp`) | Fixed: "as a large object" store banner. "ImGui demo window" stays: it is a developer/debug toggle, not product copy. Clean. |
| Shared grammar (`app_internal.hpp`, `app_operations_internal.hpp`) | Fixed: activity surface "console" → "Console". Grammar defaults clean. |

## Recorded decisions

- **"Graph" vs "pipeline".** The visual DAG editor keeps "graph" (Graph
  workspace, "execution graph" where the user is literally editing one);
  prose about the deployed, running thing says "pipeline" (Runs). Both are
  product-doc vocabulary.
- **Status words** render sentence case via `label_of` in rows and pills
  (`status_pill` uppercases itself — the one sanctioned ALL-CAPS carrier,
  along with `eyebrow`/`section_card` titles).
- **Gate verdict parentheticals** ("verdict: pass") keep the lowercase wire
  words — they are readable English mid-sentence.
- The Cluster node detail's **Raft `role` line was removed** rather than
  relabelled: it surfaced the legacy peer-table roles (follower/leader) that
  contradict the equal-peers product model; restore it only if scoped
  leadership ships as a real product concept.

## Keeping it true

The repo's grep-for-stale-terms discipline applies to UI strings: when adding
GUI copy, run the design-system self-check, route enums through `label_of`,
and check this page's rules. A new kind/status-bearing enum = one `label_of`
overload in `labels.hpp` (with a `static_assert` if its wire name is
engine-speak), same commit.

Boundaries: no new JSON format; no engine changes — `labels.hpp` and the copy
edits are presentation only (the thin-renderer rule). Issue refs: #635 (this
pass), #627 (the GUI wave epic), #630/#636 (the grammar and tones patterns it
extends).
