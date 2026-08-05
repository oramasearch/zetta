# agent — the native RLVR agent

`zetta::agent` is Orama Zetta's native **agent**: a long-horizon, tool-calling
model Orama post-trains with **RLVR** (reinforcement learning with verifiable
rewards) and ships as the base every customer forks. Like everything else in the
engine it is built native in C++26, coroutine-based and distributed — *the model
is the product*, not an embedded Python stack or a hosted model API (root
`CLAUDE.md` first principle, ADR-17).

- **[#555](https://github.com/oramasearch/zetta/issues/555)** — the base agent:
  a permissively-licensed open-weights start + Orama RLVR post-training → a
  shipped checkpoint. **This domain.**
- **[#853](https://github.com/oramasearch/zetta/issues/853)** — the per-customer
  fine-tune loop that forks the base and reuses this machinery.
- **[#849](https://github.com/oramasearch/zetta/issues/849)** — the interface to
  talk to an agent. **[#193](https://github.com/oramasearch/zetta/issues/193)** —
  the programme umbrella.
- Reference to reproduce (not embed): `homer6/blender-rl`.

The **reward** is the foundation the rest stands on: a *deterministic grader*
scores a rollout's end-state against a task's known-correct facts — a check that
runs and returns pass/fail, not a learned reward model. That is what makes an
improvement *demonstrated* rather than asserted.

## Pages

Pages land here one per shipped slice. The domain is being built bottom-up
(nibble-dev); shipped so far:

- **The reward primitive** — `EndState` + `EndStateGrader` (below).
- **The task/game** — `Task` (below).
- **Tool calls + the sandbox** — `ToolCall` + `Sandbox` (below).
- **The rollout** — `AgentPolicy` + `run_rollout` (below).
- **The reward** — the rubric + `reward_for_rollout` (below).
- **The admission gate** — `admit_corpus` (below).
- **The reference policy** — `SoftmaxPolicy` (below), the CPU policy the training loop is proven on.
- **GRPO advantages** — `group_advantages` (below).
- **The policy-gradient step** — `policy_gradient_step` (below) — reward provably rises.
- **The training loop** — `train_softmax_policy` (below) — learns to solve a real task.
- **The LLM-judge fallback** — `Judge` + `resolve_grade` (below).
- **The training-data corpus** — `curriculum` + `corpus_io`: the gated churn seed,
  graph-derived facts, the reference tool-call trajectory, JSONL, and difficulty/held-out
  (below). Runbooks: generate the data → [`training-data-runbook.md`](training-data-runbook.md);
  run the fine-tune → [`training-runbook.md`](training-runbook.md); run the trained agent →
  [`inference-runbook.md`](inference-runbook.md); **build + run a real graph from a plain-language
  ask** (`agent_run`, the base agent working end to end — no #859 needed) →
  [`build-and-run-runbook.md`](build-and-run-runbook.md).
- **GPU boxes** (#920) — [`gpu-box-runbook.md`](gpu-box-runbook.md): which box for which job,
  the Lambda instance choice (Ampere + native BF16; **not** Volta), provisioning + the CUDA
  build, and the governance line on rented hardware. **A rented GPU cannot train this model
  until #923** — the SFT driver is MLX-only.
- **The interaction contract** (#918) — [`interaction-contracts.md`](interaction-contracts.md):
  what a turn looks like, the one renderer training and serving share, the four response modes,
  and the rule that a required fact must be projectable. **Read it before changing the harness,
  the corpus, or the prompt** — the skew it prevents was diagnosed as four separate bugs.
- **Cards** (#880) — honest datasheets + model cards under [`cards/`](cards/README.md): the base
  agent's [data card](cards/datasets/base-agent.md) + [model card](cards/models/base-agent.md),
  and Auto-Suggest's [data card](cards/datasets/auto-suggest.md) +
  [model card](cards/models/auto-suggest.md). Coverage:
  [training-coverage-checklist.md](training-coverage-checklist.md).
- **Why it works** — [`rlvr-to-production.md`](rlvr-to-production.md): how the corpus →
  RLVR → the *same* correctness gate makes your production pipelines correct by construction.
- **The two models** — [`model-lifecycle.md`](model-lifecycle.md): the agent vs. the
  pipeline model, and how the pipeline model trains → checkpoints → serves.
- **Plain-language** — [`how-orama-works-plain.md`](how-orama-works-plain.md): the
  non-technical version, written to forward to a less-technical reader.
- **Auto-Suggest** — [`auto-suggest.md`](auto-suggest.md): the *separate* model (#554)
  that reads your data and suggests zero-to-many use cases as prompts for the agent — the
  "what" to the agent's "how", and the source of the games Auto-RL trains on.
- **Multi-turn training** — `SequencePolicy` + `train_sequence_policy` (below).
- **Curriculum + observability** — `train_over_corpus` (below).

## The served agent entity (shipped — data model)

- **`AgentRecord`** (`agent_record.hpp`) — the first-class **agent** a user lists, selects,
  and **chats with** ([#849](https://github.com/oramasearch/zetta/issues/849)): an id + name +
  description binding a **model** (a local checkpoint dir, a `model:<uuid>`, or a HuggingFace
  repo id `zetta pull` fetches) to a system prompt and the **tools** it may call. Stored as a
  replicated `agent:<uuid>` entity (`catalog.hpp`); non-throwing `agent_from_json` +
  `agent_schema()` are the boundary.
- **`AgentStore`** (`agent_store.hpp`) — CRUD over the `agent:` entities (put/get/list/remove
  capture into / read the CRDT document, so agents replicate + persist like every other entity).
- **The CRUD surface** — `create_agent` / `list_agents` / `get_agent` / `update_agent` /
  `delete_agent` capabilities (`api/capabilities.cpp`), exposed on **both MCP and REST** by the
  capability registry. The GUI/web chat panels build on these (chat + tool-calling in flight).

## The reward primitive (shipped)

- **`EndState`** (`end_state.hpp`) — the observable end-state of a rollout,
  projected as a set of named JSON facts. Plain data, so grading is a pure,
  reproducible function.
- **`Grade` + `EndStateGrader`** (`grader.hpp`) — the deterministic reward: score
  an observed end-state against a task's required facts by exact match, a reward
  always in `[0,1]` with a `passed` flag. A criterion-free task grades nothing —
  `score 0`, not passed — so it cannot post a passing number (the
  negative-control discipline: a grader that catches nothing is measuring
  nothing). Where a task has no deterministic check, an LLM-judge fallback grader
  is a later slice behind this same `Grade`.

## The task/game (shipped)

- **`Task`** (`task.hpp`) — one enterprise tool-calling problem the agent trains
  on and is graded against: an id, a readable description, an initial `EndState`,
  the available tool names, and the required `EndState` that is its success
  criterion. It hands out its deterministic `grader()`, and `is_trainable()` is
  true only when the criterion is verifiable (at least one required fact) — a task
  with nothing to check cannot enter the curriculum ([#554]). The tools' effects
  are wired later, in the sandbox.

[#554]: https://github.com/oramasearch/zetta/issues/554

## Tool calls + the sandbox (shipped)

- **`ToolCall`** (`tool_call.hpp`) — one agent action: a tool name + JSON
  arguments, MCP-shaped. Plain data a policy emits and the sandbox applies.
- **`Sandbox`** (`sandbox.hpp`) — the world a rollout acts in: it holds the
  current `EndState` and a set of registered tools, each an **injected effect**
  (`ToolEffect`: `world × args → ToolResult`). Applying a `ToolCall` runs that
  tool's effect and reports `Ok` / `UnknownTool` / `ToolFailed` — an unknown tool
  leaves the world untouched, never silently ignored, because a rollout that
  pretended a bad call succeeded would corrupt the reward.

## The rollout (shipped)

- **`AgentPolicy`** (`policy.hpp`) — the actor interface a rollout drives: given a
  `RolloutStep` (the task, the current state, the turn), it returns a
  `PolicyDecision` — the next `ToolCall`, or *stop*. **This is the interface the
  trained model plugs into**; tests use a scripted policy behind the same
  interface, which is what lets the whole loop be built with no model present.
- **`run_rollout`** (`rollout.hpp`) — drive a policy against a task's `Sandbox`
  for at most `max_turns` turns, applying and recording each call (including a
  failed/unknown one, honestly), and return the trajectory + the final
  `EndState`. The turn budget is a hard bound: an agent that never stops still
  terminates, so there is always a final state to reward.

## The reward (shipped)

- **The rubric** (`reward.hpp`) — `reward_for_rollout(task, rollout)` scores a
  whole rollout as a reward in `[0,1]`. The deterministic end-state grader is the
  anchor (and sets `solved`), but the training signal is a **multi-term rubric** —
  `end_state`, `right_tools` (calls that named a real tool), `right_args` (of
  those, the ones that succeeded) — combined by **active-weight renormalization**:
  only the terms a rollout exercised count, and their weights renormalize so the
  reward stays in `[0,1]` no matter which apply (the blender-rl rubric shape). The
  per-term breakdown is returned so a training run can see *why* a rollout scored
  what it did.

## The admission gate (shipped)

- **`admit_corpus`** (`admission_gate.hpp`) — a task corpus must *earn* admission
  before it trains or evaluates the agent. Each `AdmissionCase` carries a task, a
  **reference** end-state (must pass the grader) and **negative controls** (must
  fail). The gate admits the corpus only if every reference passes, every negative
  control fails, the corpus carries **at least one negative control**, and every
  task is trainable — a corpus where nothing can fail is measuring nothing
  (feat-12 step 3; the [#248](https://github.com/oramasearch/zetta/issues/248)
  gate property). It returns *every* failure, so a corpus is fixed in one pass.

## The reference policy (shipped)

- **`SoftmaxPolicy`** (`softmax_policy.hpp`) — a tiny parametric policy: one logit
  per action, a numerically-stable `softmax` for the probabilities, a
  **deterministic** `sample(uniform_draw)` (so rollouts and tests reproduce), and
  `log_prob` for the policy gradient. This is the **CPU reference policy the RLVR
  training loop is built and tested on**, following zetta's reference-first
  doctrine (the GBDT precedent): advantages, the gradient step, and the loop are
  proven against it, then a real MLX-backed model is swapped in behind the same
  interface without touching the training machinery. SYCL is deferred.

## GRPO advantages (shipped)

- **`group_advantages`** (`advantage.hpp`) — the RLVR training signal. Given the
  rewards of a *group* of rollouts on the same task, each advantage is
  `(r_i − mean) / (stddev + ε)` — how much better than the group that rollout did,
  normalized by the group's spread. The group is its own baseline, so there is no
  learned value function; and a group that all agrees (already solved, or always
  failing) yields zero advantage and does not push the weights.

## The policy-gradient step (shipped)

- **`policy_gradient_step`** (`policy_gradient.hpp`) — one REINFORCE/GRPO ascent
  step: from a batch of (sampled action, advantage), nudge the logits by
  `learning_rate · Σ advantage · (1[j==a] − π_j)` (the gradient of `log π(a)` taken
  at the sampling policy). Actions with positive advantage become more likely,
  negative less. **Repeated over a task, the reward rises** — proven directly: a
  bandit that starts uniform is trained to pick the reward-getting arm with
  >0.9 probability. This is feat-12's "watch it improve", in miniature on the CPU
  reference policy; the same loop drives a real MLX-backed model once it is swapped
  in behind `AgentPolicy`.

## The training loop (shipped)

- **`train_softmax_policy`** (`training.hpp`) — the RLVR loop: each step samples a
  group of actions, turns their rewards into GRPO advantages, and takes a
  policy-gradient step, reporting the **mean reward per step** (the learning
  curve). The **environment** (`RewardFunction`) and the **randomness**
  (`UniformSource`) are injected, so the *same loop* trains the CPU reference
  policy here and, later, a real model behind `AgentPolicy` — only those two
  change. Proven end to end: an integration test wires the reward to the real
  `Task → Sandbox → rollout → reward_for_rollout` pipeline and the loop learns to
  pick the tool call that solves the task.

## The LLM-judge fallback (shipped)

- **`Judge` + `resolve_grade`** (`judge.hpp`) — most tasks are graded
  deterministically, but some have no deterministic check; for those an **injected
  `Judge`** reads the task and the observed end-state and scores it. `resolve_grade`
  uses the deterministic grader wherever a task has a verifiable criterion and
  falls back to the judge **only** where none exists, recording which path produced
  the grade (`GradeSource`) — feat-12 step 4, since a score that is mostly judged is
  a softer claim. The deterministic grader always wins; an ungradeable task with no
  judge **fails closed** (score 0), never a silent pass. The judge is injected so
  the fallback logic is tested with a deterministic fake and no model; a real judge
  (local by default, frontier only on #555's Orama-infra runs) implements it later.

## Multi-turn training (shipped)

- **`SequencePolicy` + `train_sequence_policy`** (`sequence_policy.hpp`,
  `sequence_training.hpp`) — real tasks take several tool calls, so the policy emits
  a *sequence*. `SequencePolicy` is a fixed-horizon policy (one `SoftmaxPolicy` per
  turn); `train_sequence_policy` samples a group of full trajectories, turns their
  rewards into GRPO advantages, and applies each trajectory's advantage to **every**
  turn's policy (trajectory-level credit). Proven on an **order-enforcing** task —
  the sandbox refuses to close an unassigned ticket, so the unique solution is
  `[assign, close]` and the loop learns to assign first, then close.

## Curriculum + observability (shipped)

- **`train_over_corpus`** (`corpus_training.hpp`) — trains a policy per task across a
  corpus and returns a **per-task** report: each task's reward *before* and *after*
  and whether it improved, plus `improved_count()` and `mean_reward_after()`. This
  is the surface feat-12 step 5 asks for — you can see which games are attempted and
  how they score, and *where the base is not improving*.

## Distributed + coroutine-native (shipped)

Per zetta's first principle (native C++26, coroutine-based, cluster-native,
distributed), the training loop is not single-node synchronous — that is only the
reference. RL is embarrassingly parallel:

- **`distributed.hpp`** — the cross-node combine is an injected `AllReduce` (the
  `zetta::ml::AllReduce` pattern). `distributed_group_advantages` allreduces
  `{sum, sum-of-squares, count}` so every node normalizes by the **global** GRPO
  statistics; `distributed_policy_gradient_step` allreduce-Sums each node's local
  gradient and applies the same global update, so the policy stays bit-identical on
  every node. Proven: a step with the batch **sharded across two nodes equals the
  whole batch on one node** — single-node == multi-node by construction.
- **`distributed_training.hpp`** — the coroutine loop: within a step this node's
  rollouts **fan out via `runtime::when_all`** (each an `io_task`, so a real tool
  call's I/O overlaps), then the step allreduces. Returns `runtime::task<TrainingReport>`,
  awaited via `block_on`. `identity_allreduce` on one node is the single-node run;
  a `Collectives`-backed allreduce on N nodes is the same training, distributed.

## A neural policy trained end to end (shipped)

- **`train_linear_policy_distributed`** (`neural_training.hpp`) — the capstone: a
  real neural policy's **weights** are trained by the distributed, coroutine-native
  RLVR loop, with the forward pass on the `ComputeDevice` (the GPU under
  `MlxDevice`). The weight gradient is the policy gradient chained through the linear
  layer — `dW[k][f] += A·(1[k==a] − π_k)·x[f]`, an outer product — accumulated over
  the group and **allreduced** across the cluster, then `W += lr·dW`. Proven on a
  **contextual** task (`[1,0] → action 0`, `[0,1] → action 1`) that a tabular policy
  cannot solve: starting from `W = 0`, the policy learns to map each context to its
  rewarding action. That is a real (if small) neural network trained end to end by
  native distributed RLVR — the reference loop reproduced.

## The real transformer, trained by RLVR (shipped, MLX)

Native on MLX/Metal (`zetta::compute::model`, `-DZETTA_ENABLE_MLX=ON`), the actual
model — not a linear layer:

- **Architecture** — multi-head **causal self-attention** + GELU MLP + RMSNorm,
  a stacked pre-norm decoder with token + positional embeddings (`mlx_transformer.hpp`).
- **Real training** — `mx::value_and_grad` backprops through the whole stack; `AdamW`
  updates the weights. Proven by overfitting a sequence (loss collapses).
- **RLVR** — `policy_gradient_loss` trains the transformer *purely from a verifiable
  reward* (GRPO advantages); it learns a game without ever being shown the answer.
  blender-rl's core, in native C++26.
- **Generation** — `generate()` decodes a tool call token by token.
- **Distributed** — `distributed_adamw_step` allreduces the parameter gradients
  (data-parallel; sharded step == combined step).
- **Checkpoints** — `named_parameters()` / `load_named_parameters()` +
  `save_checkpoint`/`load_checkpoint` via **safetensors** (`safetensors.hpp`, F32 +
  BF16 decode — the HuggingFace format). A trained model round-trips through the
  format exactly.

## The Qwen3-parity model, RLVR-trained on the native autograd (shipped, device-agnostic)

The MLX slice above proved the loop on Metal. The **`zetta::autograd`** line
([`docs/autograd/`](../autograd/README.md)) then rebuilt the actual model
**device-agnostically** — on zetta's own reverse-mode autodiff, so the *same*
graph runs on CPU / CUDA / MLX (the `ComputeDevice` it is built on), which is what
lets one distributed model train across a heterogeneous cluster:

- **`QwenModel`** (`autograd/qwen.hpp`) — a **Qwen3-parity** decoder: grouped-query
  attention + **QK-norm** + **RoPE** (selectable θ) + **SwiGLU** + **RMSNorm**, with
  parameters keyed by **Hugging Face tensor names**. A synthetic checkpoint
  round-trips through a `.safetensors` file into a fresh model with identical
  logits — the exact path a real Qwen checkpoint takes.
- **`grpo_step`** (`qwen_rlvr.hpp`) — **the capstone**: blender-rl's GRPO loop over
  that model, reproduced native. Sample a group of completions
  (`QwenModel::generate`), score each with a `CompletionReward` (verifiable, a
  check that runs), normalize to `group_advantages`, and take one
  **`sequence_policy_gradient_loss`** step. Proven: the Qwen3-parity model learns
  to generate a target sequence **purely from reward, never shown the answer** —
  every gradient from the autograd's backward closures, no framework.
- **`generate_text`** (`text_generation.hpp`) — **text in, text out**: compose the
  byte-level BPE tokenizer ([`../tokenizer/`](../tokenizer/README.md)) with the
  model — encode the prompt, autoregressively decode (greedy or sampled), and turn
  just the new tokens back into text. Proven end to end: a model overfit on the
  bytes of "orama" continues the text "o" → "rama". The surface a real Qwen
  checkpoint + `BpeTokenizer::from_directory` present once both are loaded.
- **`grpo_step_text`** (`qwen_rlvr.hpp`) — **RLVR post-training in natural
  language**: encode the text prompt, run one `grpo_step`, and score each rollout by
  decoding its completion back to text for a verifiable `TextCompletionReward`.
  Proven: the model learns to continue "a" with the target text "bc" **purely from
  a text reward, never shown the answer**, and `generate_text` then produces it —
  the whole pipeline (tokenizer → model → RLVR → decoded text) closed end to end.

**To ingest an actual Qwen checkpoint** the remaining external-asset step is the
**downloaded multi-GB weights** alone — the **architecture parity** (RoPE / GQA /
SwiGLU / QK-norm, HF tensor names + shapes), the **safetensors loader** (incl. BF16,
single-file + sharded), and the **byte-level BPE tokenizer**
([`../tokenizer/`](../tokenizer/README.md), incl. `tokenizer.json` loading) are now
done and device-agnostic, so a real checkpoint's tensors line up and load by name
and its text tokenizes exactly.

## The forward pass on the compute device (shipped)

- **`LinearPolicy`** (`linear_policy.hpp`) — the bridge from the abstract policy to
  **real tensor compute**: action logits are `W · x` computed on an injected
  `ComputeDevice` via `gemm`, then softmax. The device is injected, so the *same*
  forward pass runs on `CpuDevice` (the always-present reference, default build) and
  on `MlxDevice` (the GPU) — verified to produce identical logits. A real
  transformer later is just more layers behind this same interface; the RLVR loop
  above it does not change.

This closed the reference-first training half. The **open-weights model** is now
also built and RLVR-trained — device-agnostically on the autograd (the
`QwenModel` + `grpo_step` sections above): the Qwen3 attention/weight-loading/
generation/checkpoint I/O (`load_qwen_model` ⇄ `save_qwen_model`) are done and run
on CPU / CUDA / MLX, and the **byte-level BPE tokenizer** + text I/O (`generate_text`,
`grpo_step_text`) close the loop in natural language. What remains is **external
assets + integration**: (1) the downloaded multi-GB Qwen weights (the only external
asset — the loader, tokenizer, and RLVR loop are all built and tested), and (2)
wiring the `QwenModel` behind `AgentPolicy` for the #248 tool-calling eval + the
sustained training campaign on the build box.

## The training-data corpus (shipped)

The gated seed corpus + its on-disk format (#866), grown the blender-rl way — *grow by
generation, gate by verification*. `agent/curriculum.hpp` holds the first seed:
`churn_solution_case()`, Charles's churn game (Motive UC-1) as a **discriminating**
`AdmissionCase` — its required end-state is a correct churn solution (connect →
leakage-free features → gbdt + SHAP → calibrated serve → calibration loop), built as a
real `ir::ExecutionGraph` (`build_churn_graph` / `churn_graph_facts`) and reachable as the
agent's own tool-call trajectory (`churn_sandbox` / `churn_reference_trajectory`); the
reference passes, the negatives (LLM-only, leaky, uncalibrated) each fail. `agent/corpus_io.hpp`
is the JSONL the frontier-model generators emit and the native loader consumes
(non-throwing), plus `CorpusTask` difficulty metadata + the held-out split. The generators
and how to run them: [**`training-data-runbook.md`**](training-data-runbook.md) +
[`../../scripts/README.md`](../../scripts/README.md).

Beyond the (supervised-tabular) solutions catalog the base generator also emits two built-in
**families** with fixed, discriminating targets, because a catalog-only agent borrows the
churn template for search and hallucinates a pipeline for an out-of-scope ask (both observed
when judging the trained 0.6B): **SEARCH** — *upload a dataset, index it, serve ranked
retrieval* (fulltext/semantic/hybrid; negatives are LLM-guessing, a `LIKE` scan, unranked)
— and **ABSTAIN** — an out-of-scope request the agent must **decline** (`{abstain, in_scope:
false}`) rather than fabricate a solution for. Both pass the native `admit_corpus` gate.

## Acceptance

The whole base-agent programme is validated by the manual test script
[`../testing/test-scripts/feat-12-base-agent.md`](../testing/test-scripts/feat-12-base-agent.md)
— a produced base checkpoint measurably beats the open-weights start on the
[#248](https://github.com/oramasearch/zetta/issues/248) enterprise-tool-calling
eval, the graders demonstrably catch wrong answers, and a second person
reproduces the numbers.
