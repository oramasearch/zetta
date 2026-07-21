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

## Acceptance

The whole base-agent programme is validated by the manual test script
[`../testing/test-scripts/feat-12-base-agent.md`](../testing/test-scripts/feat-12-base-agent.md)
— a produced base checkpoint measurably beats the open-weights start on the
[#248](https://github.com/oramasearch/zetta/issues/248) enterprise-tool-calling
eval, the graders demonstrably catch wrong answers, and a second person
reproduces the numbers.
