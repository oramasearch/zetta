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

This closes the reference-first training half. What remains is the **⚙️ real
model**: an `MlxDevice` compute backend + an open-weights model behind
`AgentPolicy`, then this same loop against it (see PR #857's ladder).

## Acceptance

The whole base-agent programme is validated by the manual test script
[`../testing/test-scripts/feat-12-base-agent.md`](../testing/test-scripts/feat-12-base-agent.md)
— a produced base checkpoint measurably beats the open-weights start on the
[#248](https://github.com/oramasearch/zetta/issues/248) enterprise-tool-calling
eval, the graders demonstrably catch wrong answers, and a second person
reproduces the numbers.
