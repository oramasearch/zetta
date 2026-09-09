# Nodes appear without me fighting for them · `run-1-node-bootstrap`

**Issue:** https://github.com/oramasearch/zetta/issues/215 — `[RUN-1]` Low-friction node bootstrapping
**Persona:** the person who owns the cluster. They have machines, or a cloud account, or
both. They do not want to log into each box and repeat a ritual, and they do not want to be
paged at 2am because the cluster ran out of room.
**Goal:** get nodes into the cluster with one action — and then have the cluster grow itself
when it needs to.
**Status:** **not yet runnable.** Token join ships but cannot currently be demonstrated
end to end (#784, #786); the inventory and the self-provisioning loop do not exist.

Two scenarios. Both must pass.

---

## Scenario A — I point it at my machines

### Before starting

A handful of machines you can reach, a Zetta binary available on them, and a written list of
those machines. No cluster running yet.

### Steps

1. **Write down your machines in one file** — which hosts, and anything that differs between
   them.
   → *The format is obvious enough that you get it right first time, without a reference open.*

2. **Point Zetta at that file and run one action.**
   → *One command, one action. Not a loop you run per host.*

3. **Watch it work.**
   → *You can see which machines it has reached, which have joined, and which have not — while
   it happens, not afterwards in a log.*

4. **Look at the cluster.**
   → *Every machine in your file is a member, and each sees the others.*

5. **Add a line to the file and run the same action again.**
   → *Only the new machine is brought up. The existing ones are left alone and stay healthy.*

6. **Remove a machine from the file and run it again.**
   → *It is drained and removed, not just abandoned. Your data is still intact and still
   replicated.*

7. **Take one machine away without warning** — pull it, kill it.
   → *The cluster notices, keeps serving, and tells you clearly what is missing.*

**Scenario A passes if:** one action against one file brought up every machine, adding and
removing lines did the right thing incrementally, and losing a machine did not lose data.

---

## Scenario B — it grows itself

### Before starting

A running cluster on Kubernetes with room to add machines, and permission for it to do so.
Set whatever limit you want on how far it may grow, and what it may spend.

### Steps

8. **Give the cluster more work than it has room for.**
   → *It notices it is short on capacity. You can see that it noticed, and why.*

9. **Watch what happens next, without intervening.**
   → *New hardware appears. Zetta nodes are placed on it. They join the cluster. The work
   proceeds. You did not do anything.*

10. **Check it stayed inside the limits you set** — node count, spend.
    → *It did. It did not quietly exceed them because it was busy.*

11. **Check you were asked, if you said you wanted to be asked.**
    → *If you configured approval, nothing was provisioned until you approved it. If you did
    not, it is still recorded — who or what decided to grow, when, and why.*

12. **Let the work finish and wait.**
    → *It gives the capacity back. The extra nodes leave the cluster and the hardware is
    reclaimed. Your data is still fully replicated afterwards.*

13. **Look at the bill afterwards.**
    → *You paid for the extra capacity while you were using it, and stopped when you stopped.*

**Scenario B passes if:** the cluster grew itself under load without anyone touching it,
stayed inside its limits, recorded the decision, gave the capacity back afterwards, and never
put the data at risk.

---

## Passes if

Both scenarios pass. Nodes join with one action against a written inventory, and the cluster
provisions its own hardware and its own members under load, within bounds you set, and
releases them again.

## Notes for the tester

- **Scenario B is a loop with several owners.** The signal comes from Zetta; the hardware
  comes from the Kubernetes side. If it stalls, note *where* — did Zetta ask, did anything
  respond, did hardware appear but no Zetta node land on it? The answer determines which
  issue the failure belongs to (this one, or operator#69).
- **Step 12 is the one people forget to test.** Growing is the exciting half; giving capacity
  back is the half that decides whether anyone can afford to leave it on.
- **Step 7 and step 12 both touch replication.** Check the data afterwards, properly — not
  just that the cluster is green.
- **Expect step 4 to fail right now.** Two open p1 bugs mean multi-node clusters do not
  currently form with authentication enforced (#784, #786). If it does work, say so — that is
  news.
- Record how long each scenario took end to end, and how much of that was waiting.

## Related

- `orama-operator#69` — the other half of Scenario B: the operator reads Zetta's
  capacity signal, drives Karpenter to provision hardware, and scales
  `OramaZettaCluster.spec.replicas`.
- #669 — enrollment beyond a shared join token (CSR, peer-cert pinning, revocation).
- #784, #786 — why Scenario A step 4 cannot be demonstrated today.
- [`env-3-on-prem-cluster`](env-3-on-prem-cluster.md) — the **manual** per-host version of
  Scenario A. This script is the one-action version; that one is the fallback path.
- [`env-4-ephemeral-burst`](env-4-ephemeral-burst.md) — borrowing temporary compute, which
  Scenario B resembles but is not: burst is asked for, this is decided by the cluster itself.
