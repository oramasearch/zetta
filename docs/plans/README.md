# zetta plans

Living planning docs for in-flight efforts: **where we are, what we're trying to
accomplish, and how we'd like to try to do it.** Unlike the rest of `docs/` (which
describes the system *as built*), a plan weighs options and tracks a moving target —
so it names built-vs-next explicitly and links to the code + research it rests on.
When an effort ships, fold its durable outcomes into the system docs and trim the
plan.

- [cloud-provisioning.md](cloud-provisioning.md) — the **Add-nodes cloud burst**
  effort: standing up real nodes on a cloud the operator controls (detection, async
  provisioning, SKU fallback, editable placement — built on PR
  [#608](https://github.com/oramasearch/zetta/pull/608)) and the next slice that
  makes them **join the equal-peer cluster** (cloud-init + a routable/overlay seed).
