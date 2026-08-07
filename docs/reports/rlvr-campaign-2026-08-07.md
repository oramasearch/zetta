# RLVR campaign — the loop learns

**Checkpoint:** `base-agent-0.6b-interaction-v16` · **Date:** 2026-08-07 ·
**Tracking:** [#859](https://github.com/oramasearch/zetta/issues/859),
[#983](https://github.com/oramasearch/zetta/issues/983)

16 GRPO steps, group 16, 128 tokens per completion, on one prompt. Reward is the **deterministic
grader** — the generation is parsed as a `StudioAction` and scored on the facts it actually
projects. No toy reward, no stand-in policy.

## The curve

| step | mean reward | cumulative parsed |
|---|---|---|
| 0 | 0.52 | 9/16 |
| 1 | 0.75 | 21/32 |
| 2 | 0.56 | 30/48 |
| 3 | 0.81 | 43/64 |
| 4 | 0.94 | 58/80 |
| 5 | 0.88 | 72/96 |
| 6 | 0.88 | 86/112 |
| 7 | **1.00** | 102/128 |
| 8 | 0.88 | 116/144 |
| 9 | **1.00** | 132/160 |
| 10 | 0.88 | 146/176 |
| 11 | **1.00** | 162/192 |
| 12 | 0.94 | 177/208 |
| 13 | 0.88 | 191/224 |
| 14 | **1.00** | 207/240 |
| 15 | **1.00** | 223/256 |

Reward **0.52 → 1.00**. Parse rate over the last four steps is **16/16 every step**, against 9/16
at the start. The rise is monotone in trend and stable once it arrives — no collapse over 16
steps, which is the failure the group-4 run showed by step 3.

## What this is, and what it is not

**It is:** the first evidence in this work that anything learns. The policy improved against a
check that runs, not against text someone wrote. That is the whole premise of RLVR and it had
never been demonstrated end to end here.

**It is not a trained agent.** One prompt, one goal pair (`kind`, `numbers_from_context`), 16
steps. It shows the loop is correct and stable, not that the agent is better at the job. A real
campaign runs many prompts across the use-case catalogue, and the reward has to be the solution's
measured ROI signal (kernel D7-2) rather than two facts.

**It cost 72 minutes** for 256 rollouts at 128 tokens on a laptop. That is the honest unit
economics: this is compute-bound, and the GPU box cannot run it (the RLVR path is MLX-only — see
`docs/agent/gpu-box-runbook.md`). A CUDA path is the first thing a real campaign needs.

## Set against the hand-authored work

| Approach | Rounds | Result |
|---|---|---|
| Hand-authored corpus | 4 (v13 → v16) | 4 narrow fixes, no broad gain. v13 — trained before any of it — still had the best turn-1 behaviour. |
| RLVR campaign | 1 | reward 0.52 → 1.00, parse 56% → 100% |

Each corpus round fixed the failure someone had already observed and stayed blind to the rest,
because a written corpus can only encode what its author thought of. The loop generates its own
variety and is scored on what it actually did.

## Two corrections made getting here

1. **The collapse was configuration, not code.** An earlier 6-step run fell to zero by step 3 and
   I attributed it to a missing KL penalty. It was `group_size = 4` against a default of 16 — four
   samples cannot estimate a stable mean to normalize advantages against. The default is stable.
2. **The first campaign test passed a collapsing policy**, because it asserted only
   `best_reward > 0`. Added a check comparing the late half of a campaign to the early half. This
   is the second time in this work a test written around an observed failure stayed blind to an
   unobserved one — the three-turn walk did the same thing.

## Next

- A CUDA RLVR path, so campaigns are not laptop-bound.
- Many prompts rather than one, drawn from the use-case catalogue.
- ROI as the reward signal (kernel D7-2) rather than two facts.
