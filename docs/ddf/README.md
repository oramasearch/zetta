# DDF docs

The DDF (distributed dataframe) — zetta's typed, columnar, chunked `DataFrame` over the storage
engine, every chunk a `ComputeDevice` buffer, and the op algebra built on it. Each distributed op is
SPMD: every node runs the same op over its shard and combines partials over the
[collectives](../cluster/collectives.md), so single-node == multi-node by construction. Read here
when adding or changing a dataframe op; the substrate's device seam is
[../compute/compute-device.md](../compute/compute-device.md) and the ML consumers are in
[../ml/](../ml/gbdt.md).

- [ddf.md](ddf.md) — the `DataFrame` core: typed numeric columns, chunked, device-backed — the substrate every DDF op operates on (ARC-12 / ENT-33; #380).
- [ddf-ops.md](ddf-ops.md) — the local ops: `scan` / `filter` / `project` (#381) plus `sort` / `concat` / `slice`, the single-node building blocks (#385).
- [ddf-distributed-sort.md](ddf-distributed-sort.md) — sample-partition + local sort + merge across the cluster, all-to-all shuffle over the signal plane (`DistributedSorter`, #385).
- [ddf-join.md](ddf-join.md) — the distributed inner equi-join: repartition by key via collectives, then a local hash/sort-merge join per shard (`distributed_join`, #382).
- [ddf-groupby.md](ddf-groupby.md) — distributed groupby-aggregate: per-node partials combined via gather/merge/broadcast; runnable as a recorded `ddf` ComputeJob (`group_by`, #383/#387).
- [ddf-as-of.md](ddf-as-of.md) — the window / as-of (point-in-time) op: per-entity reduce of rows with `time <= as_of` — the leakage-free featurization primitive (`as_of_aggregate`, #384).
- [ddf-featurize.md](ddf-featurize.md) — point-in-time featurization: the churn SQL shape as a DDF graph → a leakage-free feature table, distributed as a `featurize` job (`point_in_time_featurize`, #388).
