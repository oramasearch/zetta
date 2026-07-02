# The New Project screen (intent + import)

The **New Project screen** is the headed UI's front door for starting an AI
project (FEAT-1 / screen SCR-2): name the project, paste a natural-language
**intent**, and optionally **import / drag-drop an n8n workflow JSON** as a
starting artifact — all in one dialog, producing a project with its intent and
artifact captured in a single, atomic action.

It is **GUI glue only**: it wires existing core entry points together and
introduces no new core types, on-disk format, or replicated state. The acceptance
criterion — *create a project with an intent and an imported n8n artifact entirely
from the GUI* — is met by composing three already-tested pieces:

- [`Node::create_project`](../pipeline/projects.md) — the project itself.
- [`Project::capture_intent`](../pipeline/project-inputs.md) — the NL intent as an
  `intent` / `pasted` [project input](../pipeline/project-inputs.md).
- [`import_n8n_workflow_file`](../pipeline/n8n-import.md) — the n8n workflow JSON, validated
  against the n8n schema at the boundary and stored byte-for-byte as an
  `artifact` / `upload` input.

The `zetta-core` / no-SDL rule is preserved: all of this lives in
`src/gui/app.cpp`; the core APIs it calls know nothing about ImGui.

## The dialog — `App::draw_new_project_modal`

The screen is a **modal popup** (`src/gui/app.cpp`), opened from either the
**Projects** column's `New Project…` button or the **File ▸ New Project…** menu
item. `new_project_modal_requested_` asks ImGui to open it on the next frame; the
modal is issued every frame from `run()` so ImGui keeps it alive while open.
`new_project_modal_open_` mirrors its visibility and drives file-drop routing
(below).

The form has three inputs:

1. **Name** (required) — the project name. Empty → an inline `name required`
   error; a name that already exists → `a project by that name already exists`
   (the modal stays open so the name can be fixed).
2. **Intent** (optional) — a multi-line natural-language description, e.g.
   *"Score every active account for churn risk and tell me why."* Captured via
   `Project::capture_intent`, which itself **rejects empty / whitespace-only**
   text, so a blank intent simply captures nothing.
3. **n8n workflow** (optional artifact) — imported with the native
   `Import n8n workflow…` file dialog **or** by dropping a `.json` on the dialog.

**Create** runs the whole flow atomically: create the project → capture the
intent (if any) → import the staged artifact (if any), then selects the new
project and writes a one-line summary to the dashboard status line (e.g.
`Created project 'churn-risk' + intent + n8n artifact (workflow.json)`).
**Cancel** (or the window's close button) clears the form and dismisses the
dialog.

## n8n artifact: staged, validated, imported on Create

The n8n workflow is **staged**, not ingested immediately, so the whole project is
built in one atomic Create and an invalid workflow never leaves a half-built
project behind:

- Picking a file (dialog) or dropping one routes through the same
  `FileIntent::N8nArtifact` queue entry as every other file ingestion, drained on
  the main thread by `handle_incoming_files` → `App::stage_n8n_artifact`.
- Staging **validates at the boundary** with
  [`validate_n8n_workflow`](../pipeline/n8n-import.md) and shows the result inline — green
  `valid n8n workflow — N node(s)` or a red, path-qualified schema reason. This is
  fail-fast feedback; no new JSON format is introduced (it reuses the existing
  `n8n_workflow_schema`).
- A staged-but-**invalid** artifact **blocks Create** with
  `the staged n8n file is not a valid workflow — fix or clear it`, so a project is
  never created with a broken artifact. `clear` removes the staged file.
- On Create, `import_n8n_workflow_file` **re-validates at the real storage
  boundary** and stores the raw bytes (with SHA-256 + provenance) as an artifact
  input — the same path used by the API and tests.

### Drag-and-drop routing

The app already routes a dropped `.json` / `.jsonl` to row-import into the
selected table (see [large-objects.md](../data/large-objects.md)). That behavior is
unchanged. **While the New Project dialog is open**, the rest of the UI is
blocked by the modal, so a dropped file is taken as the dialog's n8n artifact
instead — `run()` enqueues it as `FileIntent::N8nArtifact` when
`new_project_modal_open_` is set, and as `FileIntent::Auto` otherwise. The
dialog's drop zone is the visual affordance for this.

## State

The screen adds only UI state to `gui::App` (no persisted or replicated state):

| Field | Purpose |
|---|---|
| `new_project_modal_requested_` | open the modal on the next frame |
| `new_project_modal_open_` | modal is visible (drives drop routing) |
| `new_project_name_` | the name field |
| `new_project_intent_` | the NL intent field |
| `new_project_artifact_path_` / `_name_` | the staged n8n file (`""` = none) |
| `new_project_artifact_status_` / `_valid_` | its validation result |
| `new_project_error_` | inline create error |

The Projects column's old inline create field was folded into this dialog, so
there is now a single front door for creating a project; select / rename / delete
of existing projects stay inline in the column.

## What's deferred

- **Compiling the intent / artifact** into an [execution graph](../pipeline/execution-graph.md)
  — the screen captures raw intent + artifact only (storage + provenance), exactly
  like the underlying ingestion slices. Parsing/compilation is a later FEAT-1
  slice.
- **More artifact types** — R1 imports n8n workflows; SQL / notebooks / schemas
  are planned ([project-inputs.md](../pipeline/project-inputs.md)).

See also FEAT-1 (`#132`), SCR-2 (`#223`), [projects.md](../pipeline/projects.md),
[project-inputs.md](../pipeline/project-inputs.md), and [n8n-import.md](../pipeline/n8n-import.md).
