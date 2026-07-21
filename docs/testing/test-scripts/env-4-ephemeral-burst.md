# Burst onto temporary machines, then get rid of them · `env-4-ephemeral-burst`

**Issue:** https://github.com/oramasearch/zetta/issues/113 — `[ENV-4]` Ephemeral burst
**Persona:** an analytics lead with a small permanent Zetta cluster and a training job that
takes longer than they want to wait. They have a cloud account and a budget. They have been
burned before by a proof-of-concept that left machines running for a month.
**Goal:** borrow extra machines for one job, get the job done faster, and be certain the
machines are gone and not billing afterwards.
**Status:** **not yet runnable** — nobody has walked this on a real cloud account. Step 9 is
known to fail intermittently (#824).

## Before starting

- A working Zetta cluster of two or three permanent machines.
- A cloud account (Azure, AWS, GCP or OCI) whose console you can open, and whose
  command-line tool you are already logged in to on one of the machines.
- A dataset large enough that training on it takes a few minutes — long enough that "faster"
  is something you can actually observe.
- A note of what is currently running in that cloud account, so you can tell afterwards what
  is new.

## Steps

1. **Look at your cluster and note how many machines are in it.**
   → *You see your permanent machines and nothing else.*

2. **Train a model on your dataset and time it.**
   → *It finishes. Write down how long it took — this is your baseline.*

3. **Set a spending cap for temporary machines**, lower than what the next step will ask for.
   → *The cap is accepted and you can see what it is set to.*

4. **Ask for more machines than that cap allows.**
   → *You are told it needs approval, and nothing is provisioned. **No machine appears in
   your cloud account at this point.** This is the important one — check the console.*

5. **Approve the request.**
   → *Only now do the temporary machines get created. You can see them in your cloud
   console, and they join the cluster within a couple of minutes.*

6. **Look at the cluster again.**
   → *The temporary machines are listed alongside the permanent ones, and are identifiable
   as temporary — you can tell which ones you will lose.*

7. **Train the same model again and time it.**
   → *The work is spread across permanent and temporary machines, and it finishes faster
   than your baseline from step 2.*

8. **Let the temporary machines go** (or release them explicitly).
   → *They leave the cluster. Your permanent machines are still there and still working.*

9. **Ask a permanent machine for a prediction from the model you just trained.**
   → *You get an answer.* **This is the step that currently fails some of the time** — see
   #824. Run it, then run it again, then again. If it works once and fails another time, the
   script has failed.

10. **Open your cloud provider's console and compare against what you noted before starting.**
    → *Nothing new is still running. No disks, no addresses, no machines left behind. Your
    bill is not still ticking.*

11. **Look at the record of what happened.**
    → *You can see that the over-cap request was blocked, who approved it, and when the
    machines were created and destroyed.*

## Passes if

The over-cap request was **blocked before anything was created**, the approved burst made the
job measurably faster, the trained model still answered from a permanent machine **every
time** after the temporary ones were gone, and the cloud account has **nothing left running**
that was not there before.

## Notes for the tester

- **Step 4 is a real test, not a formality.** If a machine appears in the cloud console before
  you approve, that is a hard fail regardless of what happens afterwards — the spending cap
  did not hold.
- **Step 9 is the known weak point.** #824 records three consecutive runs of the automated
  equivalent on 2026-07-13 going succeeded → failed → succeeded. Repeat it at least three
  times. Note how long you waited before asking, because timing may matter.
- **Step 10 is why this script exists.** No automated test can check your bill. Look at the
  actual console, not at what Zetta reports.
- If a temporary machine fails to join in step 5, note whether it was ever created at all —
  "never provisioned" and "provisioned but could not join" are different failures.
- Record the cloud provider you used. Only Azure has a documented real-world burst
  (`docs/plans/cloud-provisioning.md`); the others are less travelled.

## Related

- #824 — the intermittent failure at step 9, in the automated case `cluster.uc1_at_scale`.
- #670 — the cluster join token and how burst machines receive it; relevant if step 5 fails.
- [`env-3-on-prem-cluster`](env-3-on-prem-cluster.md) — the same "does it really work"
  question for customer-owned machines instead of borrowed ones.
