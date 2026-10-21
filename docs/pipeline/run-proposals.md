# Run proposals — the chat agent proposes, a person approves (#969)

A person asks Ask Orama, in a thread, to *"set up a churn project and tell me which accounts are
most at risk."* The agent's tools are read-only by design (a chat can describe the node but never
change it), so the agent cannot build and run anything itself. It **proposes** the run instead,
and nothing happens until a person approves.

```
chat (kind: action) ──► propose_run ──► RunProposal{proposed, impact preview}
                                         │            nothing else exists yet
                     approve_run_proposal │ reject_run_proposal
                                         ▼            (the cancel path)
             Project + Pipeline (exactly as previewed, approved) ──► run ──► report
```

This is the review-before-build state of [#885](https://github.com/oramasearch/zetta/issues/885)
(`approve_plan`) applied to a whole run, and the lifecycle invariant in `CLAUDE.md`:
*consequential actions get an impact preview with a cancel path.*

## The entity

`zetta::pipeline::RunProposal` (`include/zetta/pipeline/run_proposal.hpp`), replicated as
`run_proposal:<uuid>` CRDT keys and read straight from the document by `RunProposalStore` (the
`AgentStore` pattern), so a proposal persists and replicates with no parallel store. It carries:

| Field | What it is |
|---|---|
| `status` | `proposed` → `approved` or `rejected`, once |
| `message` | the ask, verbatim |
| `agent` | the agent whose chat turn proposed it (empty when a caller proposed directly) |
| `preview` | what approval will do: `project`, `creates_project`, `project_id` (when it lands on an existing one), `graph`, `steps`, `template`, `methodology`, `shape`, `dataset{path, description}` |
| `graph` | the compiled Pipeline, stored whole, so approval lands this graph rather than recompiling |
| `run_input` | what approval passes to `run`: `dataset_path`, `dataset`, `top_k`, `model_name`, `holdout_fraction` |
| `decided_by` / `decided_at` / `rejection_reason` | the decision |
| `project_id` / `graph_id` / `run_id` | what approval created |

## The capabilities

| Capability | Kind · right | Does |
|---|---|---|
| `propose_run` | action · `orchestration:propose_run` | compiles the ask with the same compiler as `compile_intent` / `agent_run`, mints the new project's name (or checks the named one exists), checks the dataset is on the node, and records the proposal. **Creates no Project, no Pipeline, no run.** |
| `approve_run_proposal` | job · `orchestration:approve_run_proposal` | creates exactly the previewed Project (or uses the named one) and Pipeline, approves the plan under the caller's name, then calls `run` **through the registry under the caller's own rights** (the correctness gate, the engine, the audit row). Returns the run handle; poll `get_run`. |
| `reject_run_proposal` | action · `orchestration:reject_run_proposal` | the cancel path; idempotent |
| `get_run_proposal`, `list_run_proposals` | read · `api:query` | read back a proposal, or list them newest first, optionally by status |

Every response carries the proposal view; while it is open, a `decide` block names the exact
calls that approve and reject it. That block is the approval challenge an MCP client renders:

```json
"decide": {
  "approve": {"tool": "approve_run_proposal", "input": {"proposal_id": "…"}},
  "reject":  {"tool": "reject_run_proposal",  "input": {"proposal_id": "…"}}
}
```

**Approval produces exactly what the preview said, or nothing.** It refuses (code `conflict`,
nothing created) when the project it was to create now exists, when the existing project it named
is gone or already has a Pipeline of that name, when the proposal was already approved (naming
its run) or rejected. An unknown id is `not_found` ("nothing was proposed under that id"). If the
Project and Pipeline are created but `run` refuses to start, the proposal is recorded as approved
with what it created, and the error says so and says to call `run`.

## How chat uses it

When a `chat` turn's declared kind is `action`, the handler calls `propose_run` through the
registry with the caller's own context, passing the ask, the agent, and the turn's optional
`dataset_path` / `holdout_fraction`. The reply carries `proposal` (the view above). A caller who
may not propose gets the reply with `proposal_warning` saying why none was recorded, and nothing
is created. The proposal's Pipeline is the engine's compile of the ask, as in `agent_run`; the
turn's own `pipeline` is the one the model's tool calls built, for display.

After approval, the follow-up turns need no run id: `chat` grounds on the Project whose run most
recently succeeded and reports it as `grounded_project`
([chatting-with-results](../agent/chatting-with-results.md)).

## Tests

`tests/api/test_chat_loop.cpp`, with the journeys as data in `tests/api/journeys_canonical.hpp`:

- `chat_loop_happy_path` — land a dataset, propose, **read back that no Project and no run
  exist**, approve, poll the run to `succeeded` with a `report` output, read back the Project and
  the proposal's `run_id`, refuse a second approval, read the report (ranked accounts, the landed
  row count, `held_out_auc`), and explain the top account (residual ≈ 0, contributions ordered
  by |φ|). Replayed in process (layer 1) and over real JSON-RPC (layer 2).
- `chat_loop_refusals` — an unknown proposal, a dataset the node does not hold, approving a
  rejected proposal (and nothing created), a run id that does not exist, a report from a run that
  produced none. Both layers.
- The chat legs with a scripted model (a byte-level tokenizer and an installed inference runner):
  an `action` turn proposes and creates nothing; the follow-up grounds on the run and the agent's
  `get_run_report` call returns the report to the model. Also over `POST /v1/chat`.

## Not built here

- A `system.run_proposals` read view (RULE-7, [#1378](https://github.com/oramasearch/zetta/issues/1378)). The capabilities are the read surface for now.
- Rendering a proposal in Studio ([#1377](https://github.com/oramasearch/zetta/issues/1377)).
- Teaching the model to propose with a preview and to call the introspection tools itself: the
  corpus families and the retrain in [#969](https://github.com/oramasearch/zetta/issues/969)
  steps 5 and 6.
