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

## Acceptance

The whole base-agent programme is validated by the manual test script
[`../testing/test-scripts/feat-12-base-agent.md`](../testing/test-scripts/feat-12-base-agent.md)
— a produced base checkpoint measurably beats the open-weights start on the
[#248](https://github.com/oramasearch/zetta/issues/248) enterprise-tool-calling
eval, the graders demonstrably catch wrong answers, and a second person
reproduces the numbers.
