# Chatting with what a pipeline produced

**Issue:** [#948](https://github.com/oramasearch/zetta/issues/948) step 5 ·
**Related:** [#914](https://github.com/oramasearch/zetta/issues/914) ·
[#904](https://github.com/oramasearch/zetta/issues/904)

Once a churn pipeline has run, the results are reachable from chat — the ranked at-risk
list, what drives churn, and what a new account would score. This is how to turn it on and
what each question is actually answered from.

## How it works

An agent's live tools are a **read-only allowlist** wired to the capability registry
(`kAgentTools` in `src/core/api/capabilities.cpp`). Every call the model makes goes back
through `registry.call(...)` with the **same** `ApiContext`, so the caller's own
permissions and the audit trail apply exactly as if they had made the call themselves. A
mutating capability is never on the list: a chat can describe the node, never change it.

Tools are **opt-in per agent**. Naming tools in the prompt changes what the model writes,
and the churn-trained base agent answers *worse* when told about tools it was never trained
to use — so enabling them is a data change on the agent record, not a code change.

## Turning it on

```jsonc
// update_agent
{
  "agent": "<agent-id>",
  "tools": ["list_datasets", "get_dataset", "list_models", "get_model", "score"]
}
```

| Tool | Answers |
|---|---|
| `list_datasets` · `get_dataset` | *"What's in this data?"* — tables, row counts, columns |
| `list_models` · `get_model` | *"How good is the model, and what drives churn?"* — measures and the ranked TreeSHAP drivers |
| `score` | *"Which accounts are most at risk?"* and *"What would this account score?"* |
| `get_run_report` | *"What did the run find?"* — the report a finished run emitted: quality (held-out first), drivers, the ranked accounts, `caveats` and `missing` |
| `explain` | *"Why is that account at risk?"* — one account's signed per-feature contributions, residual ≈ 0 |

**A live tool's result reaches the model** (#969). Each call runs through the registry and
its value goes back to the model as `<tool_result>{…}</tool_result>` before it answers. That
registration was dropped in `b5ca4290` (2026-07-26) while the sandbox was reworked to build
Pipelines from tool calls, which left every live tool a marker: the agent "called"
`get_run_report` and got back `{"status":"ok"}` with nothing in it. `agent::chat_sandbox`
(`src/core/agent/chat.cpp`) now wires them again, and `ChatSandbox.LiveToolsReachTheRunnerAndStepsAreNeverUnknown`
plus `ChatLoop.ChatProposesTheRunThenAFollowUpReadsItsReport` hold it. Every pipeline step kind
is also a recorded step, so an agent whose record names only read tools no longer reports its
own `connect` as an unknown tool while building it.

## The loop in one thread (#969)

1. **Ask.** A turn that asks to build something comes back `kind: action` with a `proposal`:
   the Project approving would create, the Pipeline's steps, the dataset. **Nothing exists
   yet** — no Project, no Pipeline, no run. Pass `dataset_path` (and `holdout_fraction`) on the
   turn to name the data. See [run proposals](../pipeline/run-proposals.md).
2. **Approve** with `approve_run_proposal` (or reject). It creates exactly what the preview
   named and runs it under the approver's rights.
3. **Follow up without naming the run.** With no `project` on the turn, `chat` grounds on the
   Project whose run most recently succeeded, and says which in `grounded_project`. This is
   grounding, not memory: one referent on the retrieval path the model was trained on.

`chat` also takes a JSON-body `POST /v1/chat` (#975), alongside `GET`.

## The four questions

**"Which accounts are most at risk?"** → `score` with the model and the dataset:

```jsonc
{"model": "<model-id>", "dataset_path": "motive_demo.account_features", "top_k": 10}
```

Returns entities ranked by risk, the feature columns the model expects, and the population
drivers. **The list is recomputed from the model and the table, not stored** — so it is
answerable long after the run that produced it has ended, and it cannot drift from the
model.

**"Why is that account at risk?"** → the `drivers` on the same response, or `get_model`.
These are population drivers (TreeSHAP mean |φ| across the training set), not a per-row
explanation for one account — see the limits below.

**"What would this account score?"** → `score` with rows instead of a dataset:

```jsonc
{"model": "<model-id>",
 "rows": [{"escalations_90d": 14, "billing_disputes_180d": 3, "nps_score": 2, "...": 0}],
 "entities": [9001]}
```

The row must carry every feature column the model was trained on. **A missing feature
fails** rather than being filled with zero: a zero is a real value to a gradient-boosted
model, so filling one in would score the account as though the measurement had been taken
and come back zero — confidently wrong instead of visibly broken.

**"What's our revenue next quarter?"** → the agent declines. Nothing to enable; this is
trained behaviour and it is the one that must not regress.

## What it is grounded in

`chat` grounds a churn answer in what the Project's pipeline actually produced (#914) — the
trained model's ranked drivers and its measure, which the run already wrote. Without that
grounding the agent answers from training priors and cites nothing real, which is the
failure this whole path exists to prevent.

## Limits worth knowing before demonstrating it

- **`score`'s drivers are population-level, not per-row.** For *"why is THIS account at
  risk?"* enable `explain`, which returns that account's own contributions
  ([explaining one row](../ml/explaining-one-row.md)).
- **Whether the model calls these tools by itself is a training question.** The engine wires
  them; the current checkpoint was not trained on `get_run_report` or `explain`, or on
  proposing a run and waiting. What it drives today is measured in
  [the #969 walk](../reports/chat-loop-walk-validation-2026-10-21.md).
- **The entity key is an integer.** A ranked list comes back keyed by the integer entity
  column, so joining it to business account ids is the caller's step today.
- **One open defect** ([#952](https://github.com/oramasearch/zetta/issues/952)): after a run
  finishes, the API layer stops serving read requests. The tools above are verified in the
  harness; the live click-through is blocked on that.
