# From opening the app to watching it run · `rel-7-gui-journey`

**Issue:** https://github.com/oramasearch/zetta/issues/192 — `[REL-7]` GUI as product surface
**Persona:** someone who has just been given Zetta and told it can turn their data into
something useful. They have not seen a demo, read the docs, or had anyone walk them through
it. They know their own data and their own question.
**Goal:** bring in their data, build something that answers their question, deploy it, and
watch it run — in one app, without being told what to click.
**Status:** **not yet run.** All the panels ship and the usability epic (#627) closed with
all nine children complete, but nobody has walked this cold.

## Before starting

- A build of the app, launched, on a machine with nothing set up.
- A CSV of the tester's own data — real data they understand, not a fixture.
- A question they actually want answered about it.
- **No guidance from anyone.** If the tester asks how to do something, that is a finding —
  write down the question and answer it only after noting it.

## Steps

1. **Open the app for the first time.**
   → *Within about a minute, you understand what this is for and what you are supposed to do
   first. You are not staring at an empty window wondering where to start.*

2. **Bring in your CSV.**
   → *You find the way to do it without hunting through menus. It loads, and you can see your
   data — the right number of rows, the columns named as you expect.*

3. **Look at what you just loaded.**
   → *You can tell where it came from, how big it is, and when it arrived.*

4. **Build something that addresses your question** — a pipeline, a model, a query,
   whichever the app leads you toward.
   → *The app makes the next step obvious at each stage. You are not guessing what a screen
   wants from you.*

5. **Look at the thing you built before running it.**
   → *You can see its shape — the steps, their order, what feeds what — and change it.*

6. **Deploy it.**
   → *It deploys. You are told it worked, and where it is running.*

7. **Run it and watch.**
   → *You can see progress while it happens, not just a result at the end.*

8. **When it finishes, find the answer to your question.**
   → *You get it, and you can tell whether to trust it — where the numbers came from, what
   ran, how fresh the data is.*

9. **Something will go wrong somewhere in the above.** When it does, look at what the app
   told you.
   → *The message says what happened and what to do about it, in words that mean something
   to you. Not an internal identifier or a stack trace.*

10. **Find your way back to something you saw earlier** — the dataset, the run, the model.
    → *You can navigate to it by clicking from where you are, rather than remembering which
    panel it lived in.*

11. **Close the app and reopen it.**
    → *Your work is still there.*

## Passes if

A person with no guidance got from opening the app to a trustworthy answer to their own
question about their own data — import, build, deploy, monitor — in one app, in one sitting,
without asking anyone a question.

## Notes for the tester

- **Count the questions you had to ask.** The number is the real result of this script. Zero
  is a pass. Each question is a specific finding — write down exactly what you were trying to
  do and what you could not work out.
- **Note anywhere you got stuck for more than a minute**, even if you eventually solved it.
  Solved-after-two-minutes is still a finding.
- **Note anything you did not trust.** If a number came back and you had no way to check
  whether it was right, that is a failure of step 8 regardless of whether the number was
  correct.
- **Do not skip step 9.** If nothing goes wrong naturally, break something on purpose — a
  malformed CSV, a bad value, an interrupted run — and read the message it gives you.
- Record the app version and how long the whole thing took. Time-to-first-answer for a cold
  user is the number this journey exists to move.
- **Run this with a different person each time.** Its value is entirely in the tester not
  knowing the app. A second run by the same person tests nothing.

## Related

- #627 — the usability + design epic (closed, nine children complete) whose work this journey
  is meant to validate from the outside.
- #647 — the web interface, a **different** surface; this script is the ImGui desktop app.
- Open polish items this journey may surface independently: #674 (activity view follow-ups),
  #675 (indexes panel), #676–#678 (graph run overlay), #717 / #723 (product copy).
