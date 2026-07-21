# Deploy Zetta to a managed cloud from the app · `env-5-managed-cloud`

**Issue:** https://github.com/oramasearch/zetta/issues/114 — `[ENV-5]` Managed cloud
**Persona:** an engineer with an AWS, GCP or Azure account who already uses that cloud's
command-line tool day to day. They are not a Kubernetes person. They do not want to read an
install guide, edit YAML, or run `kubectl`.
**Goal:** get a working Zetta running on their cloud, from inside the app, choosing what it
costs before it costs anything — and be able to remove it completely afterwards.
**Status:** **not yet runnable** — creating a managed cluster does not exist yet.

## Before starting

- The cloud's own command-line tool already **logged in** on this machine (`aws`, `gcloud`
  or `az`). *If it isn't, log in first — that part is the cloud provider's job, not Zetta's.*
- Permission in that account to create a Kubernetes cluster.
- A note of what already exists in the account you'll use, so you can tell afterwards what
  is new.
- A small CSV of your own data.
- **For the second run-through only:** an existing Kubernetes cluster in that account.

Run this script **twice** — once choosing a new cluster (path A), once choosing an existing
one (path B). Both must pass.

## Steps

1. **Open Zetta and go to deploy to a cloud.**
   → *It has already noticed which cloud tools you are logged in to, and offers those. You
   are not asked to paste keys or credentials anywhere.*

2. **Pick the account and the region you want to deploy to.**
   → *Your real accounts and that cloud's real regions are listed. You are choosing from
   what you actually have, not typing names in from memory.*

3. **Choose how to deploy** — either **create a new cluster** or **use a cluster I already
   have**.
   → *Both options are offered. If you pick the second, your existing clusters in that
   account and region are listed for you to choose from.*

4. **Decide the shape of it** — how many machines, how big, how many copies of your data to
   keep, and the limits on how far it may grow or shrink on its own.
   → *You are shown sensible defaults you can change, and **what it will cost** at those
   settings, before anything is created.*

5. **Approve.**
   → *Only now does anything get created. If you picked a new cluster, Zetta creates it in
   your account. You can watch progress from the app — you should not need to open the cloud
   console to find out whether it is working.*

6. **Wait for it to finish setting itself up.**
   → *Zetta installs itself into the cluster and tells you when it is ready.* **You are not
   asked to open a terminal, run `helm`, `helmfile` or `kubectl`, or follow a written install
   guide.** *If you are, this script has failed.*

7. **Connect to it and load your CSV.**
   → *You get an address and whatever credentials you need, from the app. Loading works.*

8. **Ask a question over your data.**
   → *You get the right answer.*

9. **Give it more work than the cluster comfortably handles.**
   → *It grows — but only within the limits you set in step 4. It does not exceed them, and
   it does not sit idle at maximum size once the work is done.*

10. **Tear the whole thing down from the app.**
    → *It confirms what it is about to remove, then removes it.*

11. **Open your cloud provider's console and compare against what you noted before starting.**
    → ***Nothing new is left.*** *No cluster, no node pools, no load balancers, no disks, no
    addresses. If you deployed to an existing cluster (path B), that cluster is still there
    and healthy, with Zetta's parts removed and nothing else disturbed.*

## Steps — the document

Run this part with a **second tester who has never seen the app**, working only from the
single setup document.

12. **Hand them the document and nothing else.** Give them a cloud account with the CLI
    logged in, and no other help.
    → *They reach a working, queryable Zetta by following it start to finish.*

13. **Note every question they had to ask you.**
    → *There should be none. Each one is a hole in the document.*

## Passes if

They got a working, queryable Zetta on a managed cloud **without ever running `kubectl`,
editing a YAML file, or following a manual install guide** — having chosen the size, the
replica count, the elasticity limits and seen the cost **before** anything was created — and
teardown left nothing behind that is still billing. **Both** path A (new cluster) and path B
(existing cluster) pass. **And** a second tester, given only the setup document, got there
without asking a single question.

## Notes for the tester

- **Step 6 is the whole point.** The moment you are handed an install guide and told to run
  `helmfile apply` yourself, this has failed — that is exactly today's behaviour and exactly
  what this issue is about.
- **Step 4 is the second point.** If you find out what it costs *after* it is running, that
  is a failure even if everything else works.
- **Path B is not a formality.** Deploying into a cluster someone else set up is the more
  common enterprise case. Watch for anything that assumes Zetta created the cluster itself.
- **Step 11, path B specifically:** confirm that teardown removed only Zetta's parts. If
  anything else in that cluster was touched, stop and record it — that is worse than a
  failure to install.
- Record which cloud you used. Support for the four is uneven; a pass on one is not a pass
  on the others.

## Related

- #188 `[REL-3]` — the one-button release gate over this flow.
- #215 `[RUN-1]` — node bootstrap and enrollment underneath it.
- #111 `[ENV-2]` — customer-managed Kubernetes, closed on the *manual* install path
  (`orama-cloud-v1-operator/docs/install.md`). Path B here is that same target driven from
  the app instead.
- [`env-4-ephemeral-burst`](env-4-ephemeral-burst.md) — borrowing temporary machines, once a
  cluster exists.
