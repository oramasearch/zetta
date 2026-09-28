# Graph ML (M-12)

M-12 turns a time-bounded relationship edge table into one row of joinable
features per node. It is native C++26 and uses no foreign ML runtime, hosted
model, pretrained checkpoint, tokenizer, or external weights. Weight and
tokenizer licensing is therefore not applicable.

The first implementation provides:

- in-degree, out-degree, weighted in-degree, and weighted out-degree;
- iterative PageRank;
- connected components by minimum-id label propagation;
- binary label propagation from labelled source nodes;
- local clustering coefficient; and
- deterministic node2vec-style embeddings from weighted random walks and a
  native skip-gram trainer.

The node-feature table can join to an entity table on `node`, allowing M-1 or
M-7 to consume the graph signals as ordinary numeric features.

## Input contract

The resolved dataset contract maps an edge table as follows:

| Dataset role | Edge column |
| --- | --- |
| `entity_column` | integer source node id |
| `feature_columns[0]` | integer destination node id |
| `feature_columns[1]` | non-negative numeric edge weight |
| `time_column` | finite numeric edge time |
| `label_column` | source-node binary label (`0`, `1`, or null per row) |
| `as_of` | prediction cutoff |

Repeated labels for a source must agree. Invalid weights, times, labels, or an
empty graph at the cutoff are refused.

## Outputs and report

`run_graph` writes two tables under `graph_results` and records both in the
Run output list and lineage:

- `node_features_<graph>` contains `node`, the degree variants, `page_rank`,
  `component`, `propagated_label`, `label_score`,
  `clustering_coefficient`, and `embedding_N` columns.
- `summary_<graph>` contains component count and sizes, degree minimum/mean/
  maximum, the top PageRank nodes, edge counts, and held-out node accuracy.

The registered `graph` Model repeats the numeric report metrics and stores the
deterministic node embeddings in its native weights body. `get_run_report`
returns the metrics, the source table, rows read and excluded, output-table
names, and the graph summary.

Held-out accuracy is reported only over labelled nodes selected by the stable
node-id and seed hash. Those labels are hidden from propagation before the
metric is computed. A report with `held_out_nodes: 0` carries accuracy `0` and
must not be interpreted as measured accuracy.

## Correctness rules

1. An edge participates only when `edge.time <= prediction_time`. Filtering
   happens before node discovery, feature calculation, walks, training, or
   evaluation. The report counts excluded rows.
2. A graph pipeline must name the edge time column, finite prediction cutoff,
   and an enabled clean step with the same column and cutoff.
   `GraphEdgeTimeCheck` blocks deployment when any part is absent or differs.
3. Node ids and sparse edge keys are ordered deterministically. The random-walk
   and skip-gram stages use the configured seed, so repeated runs are stable.
4. Conflicting labels for one node are refused rather than resolved by row or
   partition order.
5. One edge partition and multiple edge partitions produce the same features.

## Distribution

`run_graph_job` is the SPMD entry point. Each member supplies only its disjoint
edge partition. Members agree the sparse edge-key set, allreduce local counts,
weights, labels, and read/excluded totals, then perform an allreduce of rank mass
on every PageRank iteration. Every member receives identical global features
and the same report. The random-walk and skip-gram stage runs deterministically
on the reduced graph on each member; it does not exchange training examples.

`run_graph` is the resolved-dataset adapter used by the methodology registry.
It materializes the pipeline's selected table, invokes the same graph core,
writes the tables, registers the Model, and records lineage. Cross-process
co-launch of this table adapter is not part of this change; clustered callers
that already own edge partitions use `run_graph_job` concurrently, following
the same contract as distributed GBDT jobs.

## Running it

Compile by methodology id or by its primary solution:

```json
{
  "project": "fraud",
  "input_id": "<captured-intent-id>",
  "methodology": "M-12",
  "hints": {
    "source": "datasets",
    "table": "payments",
    "source_column": "payer_id",
    "destination_column": "payee_id",
    "weight_column": "amount",
    "time_column": "occurred_at",
    "label_column": "fraud_label",
    "prediction_time": 1735689600
  }
}
```

`compile_intent` returns an M-12 graph containing connect, cutoff filter, graph
model, and table-output steps with `runnable: true`. Call `run` with its project
and graph. When `dataset_path` is omitted, M-12 reads the connect step's own
`source.table`. `model_name` and `holdout_fraction` are optional run inputs.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Graph*:*PageRank*:*Component*:*LabelProp*'
./build/tests/zetta-tests
```

The focused tests cover known PageRank values, component membership, label
propagation, deterministic embeddings, cutoff exclusion, a three-partition
equivalence check, a real three-member allreduce job, the graph gate check, and
the registry-driven API journey.
