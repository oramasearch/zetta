# The base agent is better than what we started from · `feat-12-base-agent`

**Issue:** https://github.com/oramasearch/zetta/issues/555 — `[FEAT-12]` Build the Orama base agent
**Persona:** an Orama engineer producing the base agent that every customer will fork. They
have the open-weights starting model, the RLVR training machinery, and the general
enterprise-tool-calling games. They are deciding whether this checkpoint is good enough to
ship as "the Orama agent base."
**Goal:** produce a base checkpoint and prove it is meaningfully better at enterprise tool
calling than the open weights it started from — with evidence anyone can reproduce.
**Status:** **not yet runnable** — no training loop, no games, no grader exist in the engine.

This is base production, on Orama's own infrastructure and Orama's own games. It is **not**
the per-customer loop (that is `rel-8-agent-learning.md`, #853) and it does **not** carry the
zero-egress constraint — no customer data is involved.

## Before starting

- The chosen open-weights base model, and confirmation its license permits shipping a
  derivative.
- The general enterprise-tool-calling games and their graders.
- Compute to train on (GPU where available; the run must also be *able* to proceed on CPU).
- A held-out set of tasks the model never trained on.

## Steps

1. **Measure the open-weights model on the games, before any training.**
   → *You get a baseline score, per task, on tasks drawn from the general enterprise
   tool-calling set. This is the number to beat.*

2. **Look at the games it will train on.**
   → *Each is a real tool-calling task with a check that runs and returns pass or fail — you
   can read a task and tell what success is. Nothing is graded by opinion.*

3. **Confirm the grader can actually catch a wrong answer.**
   → *There are negative controls — tasks a broken agent must fail. Run one; it fails. A
   corpus where everything passes is measuring nothing.*

4. **Where a task has no deterministic grader, confirm the LLM-judge fallback fires** — and
   that on this base-production run a frontier judge is used deliberately.
   → *The fallback engages only where there is no deterministic check, and it is recorded
   which tasks were judged rather than checked.*

5. **Train.**
   → *You can watch it improve — which games it is attempting, how it is scoring. It runs on
   the available accelerator, and it does not silently require one that is not there.*

6. **Measure again on the same baseline tasks from step 1.**
   → *It scores higher. You can see which tasks moved and which did not.*

7. **Measure on the held-out tasks it never trained on.**
   → *Also better than the open-weights baseline. It learned enterprise tool calling, not
   the specific games.*

8. **Ship the checkpoint and have someone else reproduce the numbers.**
   → *A second engineer, from the checkpoint and the eval, gets the same result within a
   small margin.*

## Passes if

The produced base checkpoint measurably beats the open-weights starting point on the general
enterprise-tool-calling eval **and** on held-out tasks, the graders demonstrably catch wrong
answers, and a second person reproduces the numbers.

## Notes for the tester

- **Step 3 decides whether the number means anything.** Without negative controls, a base can
  post a high score while its graders catch nothing. This is the same discipline the
  reference implementation (`homer6/blender-rl`) enforces with its admission gate.
- **Step 7 is the difference between a base and an overfit.** A model that only improves on
  its training games has learned the games, not the skill every customer will fork.
- **Step 4 matters for honesty.** A base whose score is mostly LLM-judged rather than
  deterministically checked is a softer claim; record the ratio.
- **This run may use a frontier judge and external services freely** — it is Orama's own
  infrastructure and games. The zero-egress rule is the *per-customer* loop's constraint
  (#853), not this one. Do not confuse the two.
- Record the base model, its license, the size, the compute used, and the before/after
  numbers. These are the evidence behind "the Orama agent base."

## Related

- #853 — the per-customer fine-tune loop that forks this base. Its script
  (`rel-8-agent-learning.md`) is where the zero-egress, local-judge, and scoped-authority
  properties are tested.
- #248 — the shared eval corpus and admission gate this measures against.
- #554 — Auto-Suggest, which generates the *customer* curriculum (for #853); the general
  games here are Orama-curated.
- `homer6/blender-rl` — the validated RLVR reference to reproduce.
