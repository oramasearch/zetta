# Empty states — every panel says what belongs here and what to do next

The per-panel sweep of #631, on the `empty_state(headline, body, action_label)
-> bool` widget shipped in #655 (`src/gui/app_internal.hpp`; the grammar entry
is in [architecture.md](architecture.md)). The rule it enforces: **no view
shows a bare empty table on a fresh `data_dir`** — the nothing-yet case renders
one sentence of what belongs in the panel and the single next action that
produces it. Sentence case, calm, no scolding; consent-gated actions keep their
gates.

Two shapes, by where the filling flow lives:

- **The flow is elsewhere** → the empty state carries an action button and the
  click calls `navigate_to(...)` (or opens the producing dialog/modal), landing
  the user in the flow that fills the panel.
- **The flow is directly above in the same panel** (a create field, an add
  button, an editor + Run) → the empty state carries no button
  (`action_label = ""`), because a second button duplicating the affordance
  above would compete with it. The body names the affordance instead.

Master-detail panels branch **before** the scaffold: zero items renders the
empty state in place of the split (the scaffold's `empty_label` remains only as
its internal fallback). The panel still consumes its
`take_pending_selection(View::…)` deep link in the empty branch, so a stale
link can't fire after the first item arrives.

## The sweep (who says what, and where the action lands)

| Panel | Empty when | Action → lands in |
|---|---|---|
| Home · fresh node | no projects, runs, or activity | the [guided demo card](onboarding.md) (#623); once dismissed, the guided-start `empty_state` with **New project…** |
| Home · projects | no projects | none (the New project… hero button is directly above) |
| Home · recent activity | nothing recorded | none (a quiet strip; the hero button is the next step) |
| Explorer · each column | no projects / databases / tables / rows / large objects | none (each column's create/add affordance is directly above) |
| Datasets | no datasets | **Add dataset...** → the add-dataset file dialog (the header button row is skipped while empty, so the action isn't duplicated) |
| Search | no search index | **Open the graph workspace** (#655) |
| SQL query · results | nothing run yet this session | none (Run is directly above) |
| Console · transcript | no statements | none (the editor + Run are directly above) |
| Pipeline workspace | no projects at all | **New project…** → the New Project modal |
| Pipeline workspace | projects exist, none selected | **Open home** → the project list |
| Pipeline workspace · graphs | project has no graphs | none (the New graph field is directly above) |
| Deploy · deployments | nothing deployed | none (the solution picker + Deploy are directly above) |
| Activity | nothing recorded | **Open deploy** (#655) |
| Traces | nothing traced | **Open SQL query** (#655) |
| Runs | no runs | **Open deploy** → deploy a solution, then run it |
| Models | no models | **Open deploy** (a training run registers its model); the collapsed Add-a-model import affordance stays visible |
| Compute jobs | no jobs | **Open deploy** (a solution run produces the featurize/train/explain/score jobs) |
| Admin · principals | no principals | none — no in-GUI create flow yet (mutation is a later slice); the copy says principals arrive through the API or replication |
| Admin · audit log | no events | **Open console** (a console statement is the nearest governed, audited action) |

Panels that are never empty need no state: **Cluster** always contains this
node and already surfaces the copy-paste join command for adding a peer (the
"no peers" answer predates this sweep); **Cloud inspector** and **Test suite**
render built-in catalogs.

## Boundaries

- No new JSON format; presentation-only — every branch reads the same live
  engine sources the filled panel renders.
- Later slice: when a GUI create flow for principals/keys lands, the Admin
  principals empty state gains its action.

Issue refs: #631 (this sweep), #655 (the widget), #634 (the master-detail
scaffold these branches sit in front of).
