# First-run onboarding — the guided UC-1 demo (#623)

A fresh node used to open on a Home with no projects and no guidance. The
**guided demo card** (`App::draw_guided_demo_card`, `src/gui/app_onboarding.cpp`)
grows the [home dashboard](home-dashboard.md)'s guided-start slot into a
five-step walkthrough that takes a zero-knowledge user from an empty data_dir
to an **explained churn result** — the product's "point at data → working,
explained churn model" bar — with **no file dialogs** and each step **naming
the panel it just exercised**, so the app's map sticks while doing.

## The five steps

Every step composes an existing engine surface; the card invents no new
behavior path:

| Step | What it does | Engine surface | Panel it names |
|---|---|---|---|
| 1 · Load the demo dataset | Writes `demo_churn_dataset()` under `<data_dir>/demo/churn` and loads accounts + events as partitioned tables (`churn_demo_accounts` / `churn_demo_events`) | [`write_churn_dataset`](../use-cases/churn-dataset.md) + [`data::add_dataset_file`](../data/system-datasets.md) | Datasets |
| 2 · Create the churn project | Opens the [New Project modal](new-project-screen.md) pre-filled (name `churn-demo`, a churn intent) — the user clicks Create | `Node::create_project` + `Project::capture_intent` | New project |
| 3 · Watch the gate pass | Compiles the captured intent into the stored churn graph, runs the correctness gate (the Gate panel is prefilled with the graph + report), deploys to local on pass | `Project::compile_intent_to_graph` · `default_gate()` · `Node::deploy_graph` | Gate (+ Deploy) |
| 4 · Run over the cluster | Launches the real featurize → train → explain → rank pipeline off the UI thread over the live coordinator, labelled with the project + stored graph | the shared [`AsyncChurnRun`](long-running-actions.md) → [`run_churn_solution_on_node`](../use-cases/churn-solution.md) | Cluster (the [pipeline animation](cluster-pipeline-animation.md)) |
| 5 · See who is at risk and why | Renders the ranked at-risk accounts + each one's dominant driver, and deep-links into the run (trained model, metrics, ranked drivers) | the run's `ChurnSolutionResult` summary | Runs |

Step progress is **derived from live engine state every frame** (the
thin-renderer rule — nothing is stored or hand-synced): step 1 is done when
both demo tables appear in `Node::datasets()`, step 2 when a project carries a
captured `Intent` input, step 3 when a deployment of the compiled graph
exists, step 4 when a run labelled with the project has `Succeeded`. Quitting
mid-demo and replaying from Help resumes exactly where the engine state says
you are.

## Visibility, dismissal, replay

- **Auto-shows only on a truly empty node** (no projects, no runs, no
  activity) — the same condition the guided-start card used. Once any project
  exists the card never auto-shows again (the issue's "never shown again"
  rule falls out of live state, so nothing needs persisting).
- **`onboarding_active_`** (session presentation state) keeps the card on
  Home while the demo is mid-flight — its own steps create the projects/runs
  that would otherwise flip Home to the dashboard, which fills in beneath the
  card as the demo progresses.
- **Dismiss** is session-only; a dismissed empty node falls back to the plain
  guided-start `empty_state`.
- **Help · Guided demo** lists every step for later replay (the acceptance's
  "linkable from Help"): each item reopens the card on Home; because progress
  is state-derived, the named step is where the card resumes, completed steps
  keep their panel links, and the run step can run again.

## The run + its outcome

Step 4 reuses the one shared churn-run handle (`churn_run_`,
[long-running-actions.md](long-running-actions.md)) — one churn run at a time
across the Deploy panel, the Graph Workspace, and the guided demo;
`churn_run_from_onboarding_` routes the completion to the card's summary the
way `churn_run_from_deploy_` routes it to the Deploy status line. While in
flight the card renders the same live-record progress block as Deploy
(`draw_churn_run_progress`) plus a Cluster-panel link so the distributed
pipeline animation is one click away. On completion `poll_background_ops`
captures the result summary (an **action outcome**, like `deploy_status_`,
not a synced engine copy): the top at-risk accounts with risk + dominant
driver (entity keys map back to account names through the deterministic demo
dataset), the trained model's short id, and the global top driver.

## Boundaries

- **No new JSON format.** The one new engine surface is
  `zetta::usecases::churn::write_churn_dataset` — the inverse of
  [`load_churn_dataset`](../use-cases/churn-dataset.md), emitting the exact
  manifest/JSONL shape the loader reads and **validating every record against
  the same embedded draft-07 schemas before anything is written** (an invalid
  dataset fails fast; no partial files land). Round-trip covered by
  `tests/usecases/churn/test_churn_dataset.cpp`. Fixing the round trip also
  made `demo_churn_dataset()` schema-valid (its events now carry a non-empty
  `channel`, as the event schema requires).
- The GUI persists nothing: the dismiss/active flags are session presentation
  state, consistent with the app's deliberate no-persisted-preferences stance
  (`io.IniFilename = nullptr`).
- **Later slices:** a per-account click-through from the at-risk table into a
  dedicated ranked-list panel (today the ranked list is engine-computed per
  run but only summarized in GUI outside this card); replaying step copy in
  other locales.

## Pointers

- `src/gui/app_onboarding.cpp` — the card; `src/gui/app_home.cpp` — the
  visibility rule in `draw_home`; `src/gui/app.cpp` — the Help · Guided demo
  menu + the completion branch in `poll_background_ops`.
- [home-dashboard.md](home-dashboard.md) — the dashboard the demo fills in.
- [../use-cases/uc1.md](../use-cases/uc1.md) — the use case the demo teaches.
