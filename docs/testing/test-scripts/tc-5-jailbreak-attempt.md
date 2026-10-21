# Try to break out · `tc-5-jailbreak-attempt`

**Issue:** https://github.com/oramasearch/zetta/issues/252 — `[TC-5]` Sandbox escape / over-scope denied
**Persona:** someone with a legitimate, low-privilege account on a customer's cluster — an
analyst, a contractor, a compromised credential. They can submit scripts, because that is the
product. They are now trying to get something they are not entitled to.
**Goal:** escape the governance boundary. See data they should not see, do something they are
not allowed to do, take the node down, or act without leaving a trace.
**Status:** **fails today** — several escapes below are known-open (#793, #819, #759, #760,
#765, #761).

This is the **adversarial pass**. #780 is the automated regression harness for escapes we
already know about; this script is for finding the ones nobody has written a test for. A
tester who only runs the listed attempts has under-delivered — the listed ones are a warm-up.

The automated half of this script, step by step, is
[TC-5 coverage](../tc-5-coverage.md): what refuses each attempt today and the test that
proves it. Start the off-script hour from its list of open gaps.

## Before starting

- A working cluster with authentication enforced.
- A **low-privilege account**: read access to one dataset, nothing else. No admin, no
  role-management, no orchestration rights.
- A second, privileged account you can use **only to check results** — never to perform an
  attempt.
- Something to watch the network with.
- Somewhere to write down every attempt, including the ones that correctly failed.

## Steps

### Take the node down

1. **Send deliberately malformed scripts and statements.** Nonsense syntax, half-finished
   statements, absurd identifiers, wildcards where names belong, deeply nested expressions,
   very long inputs.
   → *Every one is rejected with a clear message. **The node stays up.** No crash, no restart,
   no other user's session interrupted.*

2. **Submit something that is valid but nonsensical**, and something that is enormous.
   → *Refused or handled. The node stays up and stays responsive for everyone else.*

### See what you should not see

3. **Ask directly for a table you have no rights to.**
   → *Refused.*

4. **Ask for it indirectly.** Through an aggregate, a grouping, a join, a subquery, a view, a
   filter that reveals values by which rows come back, an error message that differs depending
   on the hidden value.
   → *Still refused. Nothing leaks — not through the result, not through the error, not
   through how long it took.*

5. **Find a masked column and try to recover it** — max, min, count-by-value, ordering,
   anything that tells you about a value without printing it.
   → *You learn nothing about the hidden values.*

6. **Find data protected by a row policy and try to get around it** — including looking for a
   policy that turns out to permit everything.
   → *You see only your rows. A policy that says nothing denies; it does not allow.*

7. **Ask for the catalogue** — every user, every role, every policy, every tool, every dataset
   that exists.
   → *You see only what your own role permits. The shape of the system is not free information.*

### Do what you should not do

8. **Try to give yourself more access.** Create a token, grant yourself a role, edit a
   built-in role, become an owner.
   → *Refused, every route.*

9. **Write a script that reaches the network** — fetch a URL, use a stored connection you were
   not granted, exfiltrate a result somewhere.
   → *Refused. And with your capture running: **nothing leaves**.*

10. **Write a script that touches the machine** — read a file, write a file, run a command.
    → *Refused. The script never gets to act as the process it runs inside.*

11. **Use a credential you can see the name of but were never granted** — a saved connection
    belonging to someone else.
    → *Refused. Knowing a credential exists is not being able to use it.*

### Escape the record

12. **Do something that should be refused, then check whether the refusal was recorded.**
    → *It is: what you attempted, as whom, when.*

13. **Try to make an attempt that leaves no trace** — fail early, fail inside a longer script,
    cause an error before the audit point, flood so records are dropped.
    → *Every attempt is recorded. You cannot act invisibly.*

14. **Try to alter or delete the record of what you did.**
    → *Refused.*

### Then go off-script

15. **Spend an hour trying things nobody listed above.** Combine a legitimate capability with
    an illegitimate goal. Use one frontend to reach what another frontend guards. Do the
    normal thing in an abnormal order. Do two things at once.
    → *Write down everything you tried, especially anything that worked or behaved oddly.*

## Passes if

Every attempt was refused, the node stayed up throughout, nothing left the network, nothing
leaked through results, errors or timings, and every attempt — successful or not — is in the
record. **And** the hour of unlisted attempts produced nothing.

## Notes for the tester

- **Step 1 is currently expected to fail.** A malformed table reference can take the node down
  today (#819 — SIGABRT), and several statements crash it on a parse error (#793). Confirm
  whether they still do, and stop treating them as the only ones — go looking for siblings.
- **Never use the privileged account to perform an attempt**, only to verify results. The
  moment you use it to make something happen, the run is void.
- **Record the refusals too.** "It said no" is the result; *how* it said no is the finding —
  an error that reveals whether a hidden table exists has already leaked.
- **Timing and error text are leaks.** If a query over data you cannot see takes noticeably
  longer when the value matches, that is a channel. If the error differs, that is a channel.
- **Step 15 is the point of this script.** The listed attempts are the warm-up. The unlisted
  hour is where the value is — an escape somebody has already written a regression for is not
  a discovery.
- Record the version, the account's exact rights, and every attempt in order. A jailbreak
  report nobody can reproduce is a rumour.

## Related

- #780 — the **automated** regression harness for #759–#769. This script is the human pass
  that finds what that harness does not yet cover; anything found here should become a
  regression there.
- #843 — deny-by-default authority over graph execution; steps 9–11 are its verification.
- Known-open escapes: #793, #819 (node down), #759, #761 (escalation), #760, #765, #767
  (reading past scope), #766 (catalogue disclosure).
- [`tc-2-no-egress-capture`](tc-2-no-egress-capture.md) — step 9 overlaps its Part 2; run the
  capture from that script here.
