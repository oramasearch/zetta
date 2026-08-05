# The interaction contract

**The agent is harness code with a model attached.** The harness assembles the prompt, the model
completes it, the harness parses the completion and acts on it. The model is one function call in
the middle: `prompt → text`. Everything else — what goes in, what it means, what happens next —
is deterministic code we own.

That framing decides how the training data has to look. The corpus is not a separate artifact
that happens to be about the same subject; it is **the recorded input/output of the exact
interactions the harness runs**. It is only valid while the string the harness builds at serving
time is the string the corpus rendered at training time — not "the same shape", the same
function.

Owner: the agent domain (#918). Related: #916 (harness/protocol), #917 (the answerer model),
#555 (the base agent), #873 (the action reshape).

## Why this document exists

The rule above was violated silently for a long time, and every symptom got diagnosed
separately:

| Symptom | Diagnosed as | Actually |
|---|---|---|
| Raw JSON shown to the user | "the model outputs JSON, needs a retrain" | the harness never parsed the action it was trained to emit |
| Instructions in the prompt ignored | "prompt needs to be firmer" | the prompt was off-distribution |
| Invented accounts and scores | "add a rule telling it not to" | same |
| `Correct solution (facts):` leaking | "a training-format leak, strip it" | the model's actual trained output |

The common cause: training baked in `base_agent_system_prompt()`, while serving prepended the
`AgentRecord`'s own prompt and appended `User:` / `Assistant:` turn labels that appear in **zero**
training examples. `base_agent_system_prompt()` was referenced in exactly one place in `src/` —
the *training* renderer.

The fix is not vigilance. It is
[`tests/agent/test_interaction_contract.cpp`](../../tests/agent/test_interaction_contract.cpp)
`TrainingPromptIsByteIdenticalToServedPrompt`, which walks every generated case and asserts the
two prompts are identical. Skew is now a failing test rather than a months-long mystery.

## The response envelope

One envelope, four modes — chosen over four output formats, because separate formats are what
confound a small model:

```json
{ "kind": "action" | "answer" | "clarify" | "abstain",
  "message":         "markdown shown to the user",
  "tool_calls":      [ { "tool": "...", "arguments": {} } ],
  "execution_graph": { "nodes": [], "edges": [] } }
```

The model **declares its own mode**, so the harness's parse is a switch rather than a guess, and
the grader scores a **wrong mode as a hard failure**. Building a Pipeline for an ambiguous ask,
or answering where the retrieved facts do not support one, is exactly the discrimination the
corpus is meant to teach — making it structural beats hoping.

- Generated Python and SQL ride **inside** `execution_graph` as node attributes. No new
  top-level shape, and they are gradeable the strongest way available: **execute them**.
- Pipeline changes re-emit the **whole graph**, never a delta. A mutation is not runnable, so it
  could not be scored by the correctness gate — and the gate is the reward.
- An unknown `kind` is a boundary error, not a silent default. A document without the field
  still parses as `action`, so every pre-#918 artifact keeps its meaning.

## The triple

Each interaction is defined by three functions, and **the harness and the corpus generator call
the same three**:

| | Function | Used by |
|---|---|---|
| render | `render_interaction_prompt(system_prompt, grounding, ask)` | `build_chat_prompt` (serving) · `render_action_example` (corpus) |
| parse | `parse_studio_action(generation)` | `agent::respond` |
| grade | `response_solution_facts(action, grounding)` | admission · eval · RLVR reward |

Serving is render → generate → parse → act. Training is render → reference target → pair. Eval is
render → generate → parse → grade. Same functions, three uses.

## Retrieval is deterministic

`grounding` is what the **engine** read off the node before generating — a model's measure and
ranked drivers, an at-risk list, a schema, cluster state, a capability listing. The engine
fetches; the model only answers from what it was handed.

This is a deliberate choice, not a placeholder:

- **The reward stays well defined.** We retrieved the context, so we know ground truth, so
  "did this answer invent a number?" is decidable rather than a matter of opinion.
- **It is auditable** — the same argument as the correctness gate. For a product sold on verified
  correctness, retrieval you can inspect is a feature, not a limitation.

Model-driven retrieval (the agent choosing its own reads) is #859, a later capability and not a
dependency of this contract.

## The interaction taxonomy

| Class | Family | Mode | Graded on |
|---|---|---|---|
| 1 Build | (plan subjects) | action | the gate over the run pipeline |
| 2 Modify | `modify` | action | the gate, plus "changed only what was asked" |
| 3 Feature engineering | `feature_engineering` | action | the gate, leakage-free + point-in-time preserved |
| 4 Code in a node | `code_in_node` | action | **executing** the generated Python/SQL |
| 5 QA — results | `qa_results` | answer | numbers/entities ⊆ context; rank order |
| 6 QA — data | `qa_data` | answer | same |
| 7 Platform | `platform` | answer | same |
| 8 Governance | `governance` | action + answer | the capability actually requested |
| 9 Capability discovery | `capability_discovery` | answer | the capability exists in the supplied registry listing |
| 10 Refusal & ambiguity | `abstain`, `clarify` | abstain / clarify | correct mode; abstain recall; false-abstain; one question |

## Where the volume comes from

Not hand-listing thousands of examples — a deterministic cross product of
**subjects × contexts × phrasings** (no RNG in the product path, ADR-17).

The multiplier that matters is **context variation** in classes 5–7: hold the question fixed and
vary the retrieved facts, and the example becomes *unanswerable without reading them*. That is
the anti-fabrication signal, and it costs nothing to generate. Trailing contexts are held out as
a canary, so a model that memorized answers instead of reading the supplied facts fails outright.

Current corpus: **6,793 gated cases across 10 families, 763 canary**, every case admitted.

## A required fact must be projectable

The rule that fell out of building the grader, and the one worth remembering:

> **You cannot require a fact you cannot check.** A corpus that requires unverifiable facts
> silently stops measuring anything.

`response_solution_facts` is the projection, so it defines what a family may require. This is why
abstention does not require `in_scope` (a property of the *ask*, not of the answer), why
governance turns are graded on the capability called rather than on prose claiming an approval
happened, and why capability answers are grounded in a registry listing rather than in the
model's memory of what Orama offers.

## The anti-fabrication check

`numbers_from_context`: **every** numeric literal in the message must appear in the retrieved
context. Literals are compared **as written** — so rounding a retrieved `0.880` to `0.88` fails,
because `0.88` is a figure that was never retrieved. For a product whose pitch is verified
correctness, that is a real defect rather than a nitpick.

This closes the failure mode prompting could not: given a Project with no trained model and an
explicit instruction not to estimate, the agent previously invented a churn rate. **Refusal has
to live in the contract, not in the prompt** — a prompt raises the ceiling but cannot enforce a
floor.

## See also

[`cards/`](cards/README.md) (read the model + data card **before** changing or diagnosing a
model) · [`training-runbook.md`](training-runbook.md) ·
[`inference-runbook.md`](inference-runbook.md) · [`README.md`](README.md)
