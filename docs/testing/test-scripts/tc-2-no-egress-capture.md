# Watch the wire while it works · `tc-2-no-egress-capture`

**Issue:** https://github.com/oramasearch/zetta/issues/249 — `[TC-2]` No egress under network capture
**Persona:** a security engineer at a customer whose data is not allowed to leave their
network. They have been told "the data never leaves". They do not take that on trust; they
put a capture on the box and look.
**Goal:** watch everything Zetta sends while it does a full day's work, and decide whether
the claim is true.
**Status:** **Both parts runnable.** Part 2 verifies the deny-by-default egress policy (#75,
[`docs/governance/egress-policy.md`](../../governance/egress-policy.md)); it has not yet been run
under capture.

This script has two parts and they answer different questions. **Part 1: what does it send
today?** **Part 2: can it be told not to?**

## Before starting

- A machine you can run Zetta on and capture traffic from.
- A capture tool you trust, running outside the product.
- Some data to ingest, and a pipeline to build, deploy and run.
- A list of addresses you consider legitimate — your own warehouse, your own cloud's API —
  so you can tell expected traffic from unexpected traffic.

---

## Part 1 — what leaves today (runnable now)

1. **Start capturing everything leaving the machine. Then start Zetta.**
   → *Note anything it sends before you have asked it to do anything at all. Startup traffic
   is traffic.*

2. **Ingest your data.**
   → *Traffic goes to your source and nowhere else.*

3. **Build a pipeline, deploy it, and run it.**
   → *Watch the whole cycle. Note every destination.*

4. **Ask a question over the results.**
   → *Note anything that leaves while answering.*

5. **Leave it running idle for a while.**
   → *Note anything periodic — telemetry, update checks, heartbeats to anywhere that is not
   your own cluster.*

6. **Now go through your capture and sort every destination into: my own cluster, my own
   sources, my own cloud's API, or something else.**
   → *Every single one falls into the first three. The "something else" pile is empty.*

7. **For anything in the "something else" pile, find out what sent it and why.**
   → *You can trace it to a specific feature and a specific decision. Nothing is unexplained.*

**Part 1 passes if:** across startup, ingest, build, deploy, run, query and idle, nothing
left the machine except to destinations the customer chose, and every one of those is
explainable.

**Part 1 produces a report** at `docs/reports/` listing every destination observed, what
caused it, and whether it was expected. That report is the substantiation for the public
claim — and if the pile is not empty, it is the bug list.

---

## Part 2 — can it be told not to? (#75)

Egress is denied by default: start the node with no `--egress-allow` and no `ZETTA_EGRESS_ALLOW`
(and no Clerk issuer, which would be allowed for sign-in). See
[`docs/governance/egress-policy.md`](../../governance/egress-policy.md).

8. **Turn egress off** — start the node with no allowlist.
   → *It prints `zetta: egress deny (loopback only)` at startup.*

9. **Repeat steps 2–5 with egress denied.**
   → *The things that need the network fail cleanly, saying so. Nothing sneaks out anyway.*

10. **Allow exactly one destination — your warehouse — and nothing else**
    (`--egress-allow <account>.snowflakecomputing.com:443`, for example).
    → *Ingest works. Everything else that would have gone out is still refused.*

11. **Try to make it reach somewhere not on the list**, by whatever means the product offers.
    → *You cannot. Refused, and recorded.*

12. **Read back the record of what was refused** (the audit trail's `egress_denied` events).
    → *Every denial is there — what was attempted, by what, when.* The node records the
    destination and the time; which run asked is not recorded yet (#843).

**Part 2 passes if:** with egress denied, nothing left the machine at all; with one
destination allowed, only that destination was reached; and every refusal was recorded.

---

## Passes if

Both parts pass. What leaves today is fully explained and confined to destinations the
customer chose, **and** the product can be told to deny egress and actually honours it.

## Notes for the tester

- **Capture from outside the product.** A tool that asks Zetta what it sent is not evidence.
  The whole point is to look at the wire independently.
- **Step 1 and step 5 are where surprises live.** Nobody instruments startup and idle, and
  those are exactly where telemetry and update checks hide.
- **Do not accept "that destination is fine" without tracing it.** Step 7 exists because an
  explainable connection and an unexplained one look identical in a capture.
- **Run Part 1 as well as Part 2.** We are already making this claim publicly
  — `docs/product/positioning.md:9` cites TC-2 by name as what makes "the data never leaves"
  testable. Part 1 either substantiates that sentence or tells us to change it.
- Record the product version, the platform, and every destination you saw — including the
  legitimate ones. The full list is more useful than a pass/fail.
- Known outbound paths to expect and account for: connector queries over HTTP, the cloud CLI
  subprocesses used for provisioning and inspection, and any LLM-backed feature you have
  enabled.

## Related

- #75 `[CTL-4]` — the deny-by-default egress control that Part 2 verifies
  ([`docs/governance/egress-policy.md`](../../governance/egress-policy.md)).
- `docs/product/positioning.md:9` — the public claim this test substantiates.
- #843 — authority over graph execution, the other half of "what can this thing reach".
