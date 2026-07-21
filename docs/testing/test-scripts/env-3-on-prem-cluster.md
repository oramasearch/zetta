# Stand up a Zetta cluster on our own servers · `env-3-on-prem-cluster`

**Issue:** https://github.com/oramasearch/zetta/issues/112 — `[ENV-3]` Customer VPC / on-prem
**Persona:** a data engineer at a company that runs its own servers. They have three Linux
machines in their own network. They do not use Kubernetes, they have no cloud account
attached to this, and their data is not allowed to leave the building.
**Goal:** get Zetta running across all three machines and answer a question with it, without
handing anything to a cloud provider.
**Status:** **not yet runnable** — no one has attempted this end to end. Everything below is
what "done" means for this issue.

## Before starting

- Three Linux machines you can log in to, on the same network, that can reach each other.
- The Zetta program on each of them.
- Nothing else — no Kubernetes, no `kubectl`, no cloud login, no internet access required.
- A small CSV file of your own data on one of the machines (a few thousand rows is plenty).

## Steps

1. **Start Zetta on the first machine**, telling it to run without a screen and where to keep
   its data.
   → *It reports that it is running, prints an admin token once, and prints a join token for
   other machines to use. Nothing asks you to log in to anything external.*

2. **Write down the join token and the first machine's address and port.**
   → *Both are shown in the startup output. You should not have to go hunting in a config
   file or a log for them.*

3. **Start Zetta on the second machine**, pointing it at the first machine and giving it the
   join token.
   → *It reports that it started and joined. Within a few seconds the first machine shows it
   has one peer.*

4. **Start Zetta on the third machine** the same way.
   → *It joins too. Each of the three machines now reports that it can see the other two.*

5. **Ask any one of the machines to list the cluster members.**
   → *All three machines are listed, from whichever one you ask. They are equals — there is
   no "main" machine you have to go through.*

6. **Load your CSV on one machine and ask a question over it** — a count, a filter, a
   grouping. Something you already know the answer to.
   → *You get the right answer back.*

7. **Ask the same question from a different machine.**
   → *You get the same answer, without having copied the file there yourself.*

8. **Stop Zetta on one machine, then ask the question again from another.**
   → *You still get an answer. Losing one machine did not take the cluster down.*

9. **Start that machine back up.**
   → *It rejoins on its own. You do not have to re-issue the join token or repeat setup.*

## Passes if

All three machines formed a single working cluster — with no Kubernetes, no kubeconfig, and
no cloud account anywhere in the process — a question answered correctly from any of them,
and the cluster kept working when one machine went away and came back.

## Notes for the tester

- **The interesting failure is step 3.** The join-token path has only ever been exercised
  with machines started by a provisioner or by the operator inside Kubernetes. Doing it by
  hand on plain servers is the part nobody has confirmed.
- **Watch for anything that assumes a cloud or a cluster manager.** If a step only works
  because a `kubectl` context happens to be configured, that is a failure of this script even
  if it appears to succeed.
- **Watch for anything reaching the internet.** This customer's premise is that data does not
  leave. If the product phones out during setup, note exactly when.
- If a step needs a flag that is not discoverable from the program's own help output, write
  down which one — needing insider knowledge to get a cluster up is a finding, not a
  workaround.
- Capture what you saw at the step that failed, and how long you waited before deciding it
  had failed.

## Related

- #114 `[ENV-5]`, #188 `[REL-3]`, #215 `[RUN-1]` — provisioning a managed cluster and
  one-action bring-up. Out of scope here: this script is the *manual* path on machines the
  customer already has.
- `orama-cloud-v1-operator/docs/install.md` — the install runbook for the Kubernetes path.
  Its first prerequisite is a Kubernetes cluster, which is exactly what this script must
  work **without**.
