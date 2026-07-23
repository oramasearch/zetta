# Runbook — inference with the trained base agent

How to run the trained base-agent checkpoint on user inputs and read its output. This is
the step *after* [training](training-runbook.md) produces a checkpoint — it exercises the
model, it does not change weights. For *why* the agent produces what it does and where it
fits in the pipeline, see [`rlvr-to-production.md`](rlvr-to-production.md) and
[`model-lifecycle.md`](model-lifecycle.md).

> **Status (2026-07-22).** The 0.6B base agent is trained (`data/checkpoints/base-agent-0.6b/`,
> loss 4.62 → 0.72) and generates correct solution specs from plain-language intents. This
> runbook drives it via the fused MLX inference path. Turning a generated solution spec into
> the full tool-call trajectory that *builds* the execution graph is the RLVR fast-follow
> ([#859](https://github.com/oramasearch/zetta/issues/859)).

## Prerequisites

- **A checkpoint:** `data/checkpoints/base-agent-0.6b/` (the trained base agent) — or any
  Qwen3 checkpoint dir with `config.json` + `model.safetensors` + the tokenizer files. Point
  `ZETTA_QWEN_DIR` at it. To compare the trained agent against the untrained start, point it
  at the original open-weights dir instead.
- **The build:** MLX-on for the fast fused GPU path
  (`cmake -S . -B build -DZETTA_ENABLE_MLX=ON`). The CPU path (`QwenInference`) produces the
  identical logits, just slower.

## The prompt format (must match training)

The base agent was SFT'd on `<intent>` **+ the literal cue** `\n\nCorrect solution (facts):\n`
→ the required-facts solution JSON. **Inference must use the same prompt** — the cue is what
tells the model to emit a solution spec rather than continue the prose:

```
I want to predict which customers are about to cancel.

Correct solution (facts):
```

The model then generates a JSON object of solution facts. A prompt without the cue drifts
(it has no signal that a solution is wanted); a differently-worded cue degrades — keep it
byte-identical to the training render in
[`SftBaseAgentOnCorpus`](../../tests/tokenizer/test_qwen_real_checkpoint.cpp).

## Step 1 — generate from user inputs

The `GenerateFromTrainedBaseAgent` driver (a guarded test in
`tests/tokenizer/test_qwen_real_checkpoint.cpp`) loads the checkpoint via the fused
`MlxQwenInference`, feeds a set of realistic user inputs through the training prompt, and
prints each greedy generation. Inspection only — no asserts.

```bash
ZETTA_QWEN_DIR=data/checkpoints/base-agent-0.6b \
  ./build/tests/zetta-tests --gtest_filter='QwenRealCheckpoint.GenerateFromTrainedBaseAgent'
```

To edit the inputs, change the `user_inputs` list in that test. To run your *own* one-off
prompt, the minimal native path is:

```cpp
auto tokenizer = BpeTokenizer::from_directory(dir);       // tokenizer.json + config
auto model = MlxQwenInference::from_directory(dir);        // config.json + model.safetensors
model->reset();                                            // clear the KV cache per prompt
const std::string prompt = user_input + "\n\nCorrect solution (facts):\n";
const std::vector<int> out = model->generate_greedy(tokenizer->encode(prompt), /*max_new=*/80);
std::cerr << tokenizer->decode(out) << "\n";               // the generated continuation
```

`reset()` before **every** prompt — `generate_greedy` prefills into the KV cache, so a stale
cache from a prior prompt corrupts the next. `generate_greedy` returns the *generated* tokens
(the continuation), not the prompt.

## Step 2 — judge the output

Good output is a valid JSON object of solution facts with the **right** properties for the
intent. From the trained 0.6B:

| User input | Agent output (abridged) |
|---|---|
| "predict which customers are about to cancel" | `model_kind: supervised_binary_classifier, leakage_free, calibrated, explanation: SHAP, feature: point_in_time, outcome: churn_next_30_days` |
| "score sales leads by likelihood to close" | `classification, calibrated, SHAP, point_in_time, as_of_joins, serves_scores` |
| "prioritize overdue-invoice collections" | `classification, calibrated, leakage_free, outcome: payment_status, methodology: M-1, policy_engine: C-2` |
| "Gemini reads tickets, guesses churn — make it better" | `supervised_binary_classifier, calibrated, leakage_free, point_in_time, temporal_validation` |

What to look for:

- **Right model kind** — churn/lead/collections → a supervised classifier; forecasting → a
  probabilistic/quantile forecast. Not "call an LLM".
- **The correctness invariants** — `leakage_free`, `point_in_time` features, `calibrated`,
  an explanation layer (SHAP). These are the facts the admission-gate grader checks.
- **Catalog grounding** — references to `M-n` methodologies / `C-n` components mean it
  learned the structured catalog, not just surface phrasings.
- **The LLM→trained-model move** — a current-state paste that describes an LLM prompt should
  come back as a *trained* model. That is the whole thesis.

## Step 3 — grade it (optional, deterministic)

To score a generation instead of eyeballing it, parse the emitted JSON into an `EndState` and
run it through the **same** grader the corpus is gated on — a fact-for-fact check against the
task's `required` end-state (`agent/grader.hpp`, `EndStateGrader`). That is the identical
authority used by `admit_corpus` and by the RLVR reward, so an inference score and a training
reward mean the same thing. (A batch grader over generations is a natural small follow-up; for
now the admission-gate tests are the worked example of driving that grader.)

## Step 4 — build + run a graph from the ask (`agent_run`, #872)

The base agent is usable **right away** to do real work — it does **not** need the RLVR
tool-calling fine-tune ([#859](https://github.com/oramasearch/zetta/issues/859)) first. The
`agent_run` capability closes the loop end to end from plain language:

1. the agent reads the ask and recognizes the use case (the `respond` path above),
2. the engine compiles the same ask into a real `ir::ExecutionGraph`
   (`pipeline::compile_intent` — the churn-shape heuristic), stored on a project,
3. the graph runs through the gate + the real engine (delegates to `run`; a churn graph
   trains a churn model over the live cluster).

```bash
# one call: the agent builds AND runs a churn graph for Charles
curl -s -X POST http://127.0.0.1:4800/v1/agent_run \
  -H "Authorization: Bearer $ZETTA_TOKEN" -H 'Content-Type: application/json' \
  -d '{"agent":"<agent-id>","message":"predict which customers are about to churn, with calibrated scores and explanations","top_k":10}'
# -> {reply, shape:"churn", steps:[connect..act], run:{run_id, engine:"churn", status:"running"}}
# poll get_run <run_id> -> succeeded, outputs include a trained churn model
```

It is the same capability behind the **"Build & run"** button in the GUI + web Chat views
(`docs/gui/agents.md`). Verified live: the 0.6B base agent recognized the churn ask, the engine
built the 7-step `connect → clean → model → index → serve → act` graph, and the run succeeded
with a trained churn model.

## Scope — engine-compiled today, agent-emitted next (#859)

`agent_run` builds the graph by **compiling the agent's recognized intent** (`compile_intent`).
The complementary path — the agent **emitting the tool-call trajectory** itself, step by step,
driving `Sandbox` → `ir::ExecutionGraph` graded by `grpo_step` — is
[#859](https://github.com/oramasearch/zetta/issues/859)'s RLVR fine-tune. #859 is an
**enhancement** (a more autonomous agent), not a prerequisite: the base agent builds + runs
graphs today via `agent_run`.

## Related

- [training-runbook.md](training-runbook.md) — produce the checkpoint this runbook loads.
- [training-data-runbook.md](training-data-runbook.md) — generate the corpus it was trained on.
- **native-ai-dev** skill — `MlxQwenInference`, the KV cache, the fused decode path.
- [#555](https://github.com/oramasearch/zetta/issues/555) (the base agent) ·
  [#872](https://github.com/oramasearch/zetta/issues/872) (`agent_run` — build + run a graph
  from the ask, no #859 needed) ·
  [#859](https://github.com/oramasearch/zetta/issues/859) (RLVR trajectory — the agent emits
  the graph itself).
