# From a sentence to something running · `tc-8-intent-to-deployed`

**Issue:** https://github.com/oramasearch/zetta/issues/255 — `[TC-8]` Intent→deployed latency
**Persona:** someone with a question about their data and a deadline. They are not measuring
Zetta; they are trying to get something working before a meeting.
**Goal:** describe what they want in a sentence and end up with a deployed, running pipeline
— and find out how long that actually takes.
**Status:** **not yet run** — the path is built (capture intent → compile → graph → deploy →
run); nobody has timed it end to end.

The claim this measures is the product's headline: **intent → deployed intelligence, from
quarters to hours.** This script produces the number behind it.

## Before starting

- A working Zetta with the tester's own data connected.
- A real question they want answered — theirs, not a fixture.
- A stopwatch, and somewhere to record a time at each step.
- **A tester who has not done this before.** A second run by the same person measures typing
  speed, not the product.

## Steps

Record the clock at every step. The gaps between them are the result.

1. **Start the clock. Describe what you want, in a sentence, in your own words.**
   → *It is accepted. You are not made to translate it into a form or a schema first.*

2. **Let it propose something.**
   → *You get a pipeline back, and it is recognisably about what you asked for.*

3. **Read what it proposed.**
   → *You can tell what it is going to do, without being an engineer.*

4. **Change something you disagree with.**
   → *You can, and it does not throw away the rest of what it built.*

5. **Deploy it.**
   → *It deploys.*

6. **Run it.**
   → *It runs, and finishes.*

7. **Read the answer. Stop the clock.**
   → *You have an answer to the question you asked in step 1, and you can tell whether to
   trust it.*

8. **Now count how much of that time was you, and how much was it.**
   → *Separate the two. Thinking, typing and reading are yours; compiling, deploying and
   running are the product's.*

9. **Do it again with a second, different question.**
   → *Faster than the first, because you now know the path. Record both numbers — the first
   is what a new user experiences, the second is what a returning one does.*

## Passes if

A person who had not done this before got from one sentence to a deployed, running pipeline
producing a trustworthy answer, without help — and the total time is recorded, split into
time spent by the person and time spent by the product.

There is no target number here. **The point is to have an honest one**, measured the way a
customer would experience it, so the claim we make publicly is one we have checked.

## Notes for the tester

- **Do not clean up the timing.** Include the parts where you were confused or re-read
  something. A number that excludes the hesitation is not the number a customer gets.
- **Step 8 is the useful split.** Product time is an engineering problem; human time is a
  design problem. A total that hides which one dominates cannot be acted on.
- **Step 3 decides whether the rest counts.** If you cannot tell what the pipeline will do
  before running it, then "deployed" was reached without understanding, and the time is
  measuring the wrong thing.
- **Record every question you had to ask**, as in the GUI journey script. Time and questions
  are the two numbers that matter for a first-run experience.
- Note the version, the size of the data, and the question you asked verbatim. A latency
  figure without the question behind it is not reproducible.
- If any step is impossible, record where you stopped and why — a partial time to the point
  of failure is still evidence.

## Related

- #846 — intent, description and goals belong to the project definition. This script starts
  at "describe what you want", which is exactly the surface that issue reshapes; expect the
  step-1 experience to change when it lands.
- [`rel-7-gui-journey`](rel-7-gui-journey.md) — the wider cold-user journey. This script is
  the timed subset of it, from intent to a running result.
- #134 `[FEAT-3]` — the correctness gate the pipeline passes on the way; #248 `[TC-1]`
  measures how often it produces a *correct* result, where this measures how fast.
