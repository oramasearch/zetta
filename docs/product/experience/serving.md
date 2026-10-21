# Serving — models and indexes as endpoints

Every trained **Model** and built **Index** is **cluster-internal by
default**, and can be **exposed as HTTP** through an ingress as a governed,
per-entity choice ([#648](https://github.com/oramasearch/zetta/issues/648)):

- `expose_model` / `expose_index` publish `/v1/models/{name}/predict` and
  `/v1/indexes/{name}/search` — the distributed batch-scoring and
  scatter-gather search paths underneath (distributed-first: an exposed
  endpoint is answered by the whole cluster, not one node).
- Governance as everywhere: exposure is an `orchestration:` capability,
  endpoint access is key-scoped, every request is a trace span, exposure
  state is visible in `system.models` / `system.indexes` and
  audited. Un-exposing is one action.

A **pipeline** is published the same way (zetta#1289): `expose_pipeline`
pins an approved pipeline behind `POST /v1/pipelines/{name}/run`, which runs
the pinned version with the caller's input, checked against the schema the
exposer declared, under the caller's own grants, rate and size limited and
audited ([../../api/serving.md](../../api/serving.md#published-pipelines-zetta1289)).

This is the `serve` step's full meaning: the pipeline doesn't just produce
artifacts — it stands them up as endpoints the customer's own systems call,
with the data and the model never leaving the cluster.

**Shipped** (#648): [../../api/serving.md](../../api/serving.md) — replicated
exposure state, `data:predict` / `data:search` key scoping, `serve:*` spans,
`exposed`/`endpoint` in `system.models` / `system.indexes`, allow + deny
journeys in-process and over loopback HTTP.
