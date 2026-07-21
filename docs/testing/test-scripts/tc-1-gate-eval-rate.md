# Can I trust the number on the box? · `tc-1-gate-eval-rate`

**Issue:** https://github.com/oramasearch/zetta/issues/248 — `[TC-1]` Correctness-gate eval rate
**Persona:** someone about to rely on Zetta generating pipelines for them — an evaluator at a
prospective customer, or our own release manager deciding whether self-service is ready.
They have been told a percentage. They want to know whether to believe it.
**Goal:** find the claim, understand exactly what it measures, and reproduce it.
**Status:** **not yet runnable** — no eval set and no measured rate exist.

## Before starting

- The published report and whatever number it states.
- A machine that can run Zetta.
- No prior involvement in producing the number. **The person who ran the eval must not be the
  person running this script.**

## Steps

1. **Find the claim.**
   → *There is one number, stated plainly, with a date and the version it was measured on.
   You do not have to assemble it from several places.*

2. **Read what it counts.**
   → *You can tell exactly what "correct" meant: that a graph compiled, that it ran, and that
   its result matched something known. Three distinct things, not one blurred one.*

3. **Look at the tasks it was measured over.**
   → *You can see them. There are enough of them to mean something, and they are recognisably
   varied — not twenty versions of the same pipeline.*

4. **Pick a task and work out, by reading it, whether it passed or failed.**
   → *You can. The check is something that runs and returns yes or no. Nobody's judgement is
   in the loop.*

5. **Look for the tasks it got wrong.**
   → *The failures are listed, not hidden. You can see what kind of thing it fails at.*

6. **Check that a wrong answer would actually be caught** — find a task where a deliberately
   broken graph is expected to fail, and confirm it does.
   → *There are negative controls. A corpus where everything passes is measuring nothing.*

7. **Run the eval yourself.**
   → *You get the same number, within a small margin. It is one command, and you did not need
   help to run it.*

8. **Add a task of your own** — something from your domain that you know the right answer to.
   → *It runs, it is graded the same way, and the number moves as you would expect.*

9. **Ask when it was last measured.**
   → *Recently, and against a version you recognise. Not a number from six months ago quoted
   as if current.*

## Passes if

An independent person found one clearly-scoped number, understood precisely what it counted,
saw both the passes and the failures, confirmed the corpus can actually catch a wrong answer,
and reproduced the figure themselves with one command.

## Notes for the tester

- **Step 6 is the one that decides whether the number means anything.** A corpus with no
  negative controls will happily report a high pass rate while catching nothing. If you cannot
  find a task that is *supposed* to fail, the eval is not trustworthy regardless of its score.
- **Step 2 matters because "correct" is three claims.** Compiled, ran, and produced the right
  answer are very different bars. A number that blurs them is marketing, not measurement.
- **Step 7 is the real test.** A figure nobody outside the team can reproduce is not a
  measurement, it is an assertion.
- **Do not skip step 3.** Twenty variations of one pipeline can produce an impressive
  percentage that says nothing about the general case. Judge the spread yourself.
- Record the number you got, how long the run took, and anything you had to ask about. This
  script is the evidence behind a claim we intend to make publicly.

## Related

- #134 `[FEAT-3]` — the correctness gate this measures (closed/completed).
- #262 `[TP-1]` — the verification plan that contains this test case.
- #193 — the agent programme. Its RLVR curriculum needs the *same* shape of artefact: tasks
  with deterministic, verifiable graders. Decision of 2026-07-20 is that this corpus is built
  once and shared, not twice.
- `docs/testing/example-scripts.md` — the existing harness where example programs already run
  parse → IR → gate → run and assert on outputs; the seed for this corpus.
