# Work with more data than the machine can hold, cheaply · `feat-7-tiered-storage`

**Issue:** https://github.com/oramasearch/zetta/issues/138 — `[FEAT-7]` Tiered storage
**Persona:** someone running a large search or analytics workload who is paying too much to
store it. They have looked at S3-backed alternatives and are comparing bills. Most of their
data is rarely touched; a small slice is hit constantly.
**Goal:** keep queries fast on the data that matters, park everything else on the cheapest
storage that still answers when asked, and pay a lot less than keeping it all hot.
**Status:** **not yet runnable** — no tiering exists. Today every record is held in memory.

## Before starting

- A dataset several times larger than the machine's memory.
- A realistic query pattern: mostly recent or popular data, occasionally something old.
- Somewhere cheap to put cold data — object storage, and an archive class of it.
- A note of what your current storage bill looks like, or what it would be keeping all of
  this on fast storage.

## Steps

1. **Load the whole dataset.**
   → *It loads. The machine does not run out of memory. This alone does not work today.*

2. **Run your normal queries** — the recent-and-popular ones.
   → *Answers come back at the speed you would expect from fast storage.*

3. **Look at what is actually held on the expensive tiers.**
   → *Only a fraction of the data. You can see how much is where — memory, fast local disk,
   slower disk, object storage, archive.*

4. **Leave it running for a realistic period** with your normal access pattern.
   → *Nothing degrades.*

5. **Look at where the data sits now.**
   → *It moved. What you query often is on fast storage; what you never touch has drifted
   down to the cheap tiers. You did not have to tell it to do this.*

6. **Query something you have not touched in a long time.**
   → *You get the correct answer. It is slower — that is fine — and it is slow within a
   bound you were told about in advance, not an unbounded stall.*

7. **Query it again immediately.**
   → *Faster the second time. Asking for cold data brought it back up.*

8. **Ask how stale each dataset is** relative to the source it came from.
   → *A real number, per dataset, that you could set an alert on.*

9. **Ask what it is costing you, broken down by tier.**
   → *An itemized answer. You can see the trade you are making.*

10. **Compare that to keeping everything on fast storage.**
    → *Materially cheaper — the whole reason for doing this.*

11. **Ask why a particular dataset ended up where it did.**
    → *An explanation you can follow and disagree with. Not a black box.*

12. **Force something to stay hot, and force something else to go cold.**
    → *It obeys, and keeps obeying — your instruction is not quietly undone by the
    placement logic later.*

## Passes if

They worked with far more data than the machine could hold, got correct answers from every
tier, saw most of their bytes drift to cheap storage without managing it by hand, paid
materially less than an all-hot layout, and could both **understand** and **override** the
placement decisions.

## Notes for the tester

- **Step 1 is today's hard wall.** The default storage engine holds every record in memory
  for reads, so a dataset bigger than RAM is not currently workable. If step 1 fails, stop —
  nothing after it is meaningful yet.
- **Step 6 is the one that decides whether this is a product or a science project.** Cold
  data that answers *eventually* is not the same as cold data that answers within a stated
  bound. Record actual times, and compare them against whatever bound you were promised.
- **Step 10 is the competitive question.** The point of the archive tier is that it is
  roughly 6× cheaper than standard object storage. If the saving does not show up in the
  itemized cost, the tiering is not earning its complexity.
- **Step 11 matters more than it looks.** Placement is intended to be driven by prediction.
  A prediction you cannot inspect is one you cannot trust with your bill.
- Record the actual sizes, timings and prices you saw. This script doubles as the evidence
  for the cost claim.

## Related

- [`env-4-ephemeral-burst`](env-4-ephemeral-burst.md) — the compute equivalent of this
  question: borrow capacity, pay only for what you used.
