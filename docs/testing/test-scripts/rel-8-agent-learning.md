# The agent gets measurably better at my tools · `rel-8-agent-learning`

**Issue:** https://github.com/oramasearch/zetta/issues/853 — per-customer fine-tune loop
(moved here from #193/#555 on 2026-07-20: this is the acceptance for the *per-customer* loop
that forks the Orama base agent; base production is #555, tested by `feat-12-base-agent.md`)
**Persona:** a platform lead at a company with its own warehouse, its own internal tools and
its own conventions. Generic models are mediocre at their stack — wrong table names, wrong
join keys, wrong tool for the job. Their data cannot leave their network.
**Goal:** end up with an agent that is measurably better at *their* environment than the one
they started with, and be able to prove it.
**Status:** **not yet runnable** — no training loop exists in the engine.

## Before starting

- A running Zetta cluster in their environment, with their real connections attached.
- A set of tasks they care about, in their own words — questions to answer, things to build.
- A note of what the stock agent currently gets right and wrong on those tasks.
- Whatever they use to watch network traffic leaving their network.

## Steps

1. **Point the agent at your environment** — your tools, your connections, your data.
   → *It reports what it can see: which tools, which data, what it can and cannot reach.*

2. **Ask it to measure where it stands today**, before any training.
   → *You get a baseline score on tasks drawn from your own environment, and you can see the
   individual tasks and how it did on each. Not a single opaque number.*

3. **Look at the tasks it intends to practise on.**
   → *They are recognisably about your environment — your tables, your tools — and each one
   has a check that is obviously either passed or failed. You can tell, reading it, what
   would count as success. Nothing is graded by opinion.*

4. **Disagree with one of them and remove it. Add one of your own.**
   → *It accepts both. Your task is practised and graded like the rest.*

5. **Let it train.**
   → *You can see progress: which tasks it is attempting, how it is scoring, whether it is
   improving. It does not go dark for hours.*

6. **While it runs, watch your network boundary.**
   → ***Nothing leaves.*** *No data, no prompts, no traces, no model weights. This is the
   whole premise — check it, do not assume it.*

7. **Measure again on the same baseline tasks.**
   → *It scores better than step 2. You can see which tasks improved and which did not.*

8. **Give it a real task you care about — one it never practised on.**
   → *It does better than the stock agent did on the same task. Improvement generalised
   beyond the practice set; it did not just memorise the games.*

9. **Ask what it learned and why it improved.**
   → *You get something inspectable — which tasks moved the score, what changed. Not "trust
   me, it is better now".*

10. **Decide it got worse at something and roll back to the previous agent.**
    → *You can. The previous one is still there, and switching back is quick.*

11. **Try to share the improved agent with a colleague's separate environment.**
    → *What is shareable and what is not is clear. Anything learned from your data does not
    silently walk out of your organisation.*

## Passes if

They ended with an agent that scores measurably higher on tasks drawn from their own
environment, improved on a real task it never practised, produced evidence they could
inspect, never sent anything outside their network, and could roll back.

## Notes for the tester

- **Step 6 is the premise of the product.** If anything leaves the network during training,
  stop the run and record exactly what left and when. Nothing else in this script matters if
  that fails.
- **Step 8 is the difference between learning and memorising.** An agent that improves only
  on its practice tasks has not learned your environment — it has learned the games. Pick a
  task deliberately unlike the practice set.
- **Step 3 is the one that decides whether this is trustworthy.** A task whose success
  criterion you cannot understand by reading it is a task you cannot audit. Rewards must be
  verifiable — a check that runs and returns pass or fail — not a judgement.
- **Step 10 matters more than it sounds.** A system that can only move forward is one nobody
  will turn on in production.
- Record the baseline and final scores, per task, and how long training took on what
  hardware. Those numbers are the evidence for every claim made about this feature.
- If the agent is given tools that can change things — write to a table, call an external
  system — note carefully what it was allowed to do while practising. See #843.

## Related

- Reference implementation, RLVR against a deterministic grader: blender-rl (an earlier RLVR prototype, not public) —
  agent emits code, a headless subprocess runs it, exported state is scored `[0,1]` by a
  layered rubric, with an admission gate carrying negative controls.
- #843 — deny-by-default authority over execution. Practising with the customer's real tools
  and data is the highest-stakes version of that problem.
- #75 `[CTL-4]` — egress denied by default, which is what step 6 tests.
- #554 / #555 / #556 — the user-facing surfaces of this program.
