# Simulated-Python frontend → ExecutionGraph IR

zetta accepts input "from Postgres SQL to Python code." A **language frontend**
(ENT-38 / ADR-33) turns a source snippet in some input language into the
[ExecutionGraph IR](execution-graph.md), the IR the [gate](gate.md),
[deploy](deployments.md), and [run](execution.md) all build on. It is the
IR-targeting analogue of the query layer's
[`zetta::query::Dialect`](../data/query.md), which turns SQL text into the generic query
AST: add support for another input language by implementing the interface, and
everything downstream is reused as-is.

The first frontend is the **simulated-Python** frontend
(`include/zetta/input/python_frontend.hpp`, `zetta::input::PythonFrontend`): it
parses a small, well-defined subset of Python and lowers it to the IR. It is
**not** a Python interpreter — it runs none of the source language and none of its
libraries. It recognizes the *shape* of a pipeline written in Python (the
[new-input principle](../../.claude/skills/new-input/SKILL.md): re-implement, don't
run) and maps it to typed IR steps, the way
[`compile_n8n_workflow`](n8n-compile.md) maps an n8n workflow.

**Acceptance (issue #520):** a sample script

```python
ds = load_dataset("saas-churn")
m  = train(ds, target="outcome")
```

parses to a valid IR graph — two steps (`load_dataset` → connect, `train` →
model) joined by a data-flow edge, which round-trips through
[`ir::from_json`](execution-graph.md), passes the baseline [correctness
gate](gate.md), and executes through `Node::run_graph` like any other frontend's.

## The supported subset

| Construct | Example |
|---|---|
| assignment | `ds = load_dataset("…")` |
| function call | `train(ds, target="outcome")` |
| attribute / method access | `df.dropna()`, `a.b.c()` |
| method chaining | `read_csv("x").dropna().fillna(0)` |
| literals | strings (incl. triple-quoted + `f`/`r`/`b` prefixes), ints, floats, `True`/`False`/`None`, lists, dicts |
| `import` / `from … import` | recorded as nothing — no step produced |
| comments, `;`, newlines, implicit line-joining in brackets | handled |

Everything else — `def` / `class` / `if` / `for` / `while` / binary operators /
comprehensions — is **explicitly rejected** with a reason (`unsupported Python
construct '…'`), not silently mis-parsed. This is a *minimal* frontend; the
grammar grows as more samples arrive.

## The interface

`include/zetta/input/language_frontend.hpp` (namespace `zetta::input`):

```cpp
struct FrontendResult {
    ir::ExecutionGraph graph;  // valid only when ok()
    std::string error;         // empty on success
    bool ok() const { return error.empty(); }
};

class LanguageFrontend {
public:
    virtual const char* name() const = 0;                       // "python"
    virtual FrontendResult compile(std::string_view source) const = 0;
};
```

`PythonFrontend` is the first implementation (stateless — construct one and call
`compile`, the same shape as `query::PostgresDialect`):

```cpp
zetta::input::FrontendResult result =
    zetta::input::PythonFrontend{}.compile(source);
if (!result.ok()) {
    // result.error: "unterminated string literal" /
    //               "unsupported Python construct 'for'" /
    //               "no pipeline steps recognized" / …
}
node.run_graph(result.graph);  // executes like any stored graph
```

`compile` is **non-throwing**, matching zetta's convention for zetta-core
([`ir::from_json`](execution-graph.md) / [`compile_n8n_workflow`](n8n-compile.md)
are non-throwing too — the library is compiled with `-fexperimental-library` and
must not unwind exceptions across its boundary). The whole parser propagates a
failure as an error string and bails; nothing throws.

## How a script lowers to the IR

Two rules:

1. **Every call becomes a typed step.** Its `StepKind` comes from the
   called function/method name via `python_step_kind` (the verb table below). A
   name the table doesn't know defaults to `StepKind::Clean` (a generic
   transform), so an *arbitrary* script still lowers to a well-formed graph rather
   than failing — known verbs still get their precise stage. The node's `id` is a
   deterministic, unique slug from the verb (`train`, then `train_2`, … for
   repeats); its `name` is the verb.
2. **Data flow becomes `DataFlow` edges.** An assignment binds a variable to the
   node a call produces. A later call that references that variable — as an
   argument, inside a list/dict argument, or as a **method receiver** — gets an
   edge from the producer node. So `a = f(); b = g(a)` is `f → g`, and a chain
   `read_csv(…).dropna().fillna(0)` is `read_csv → dropna → fillna`.

Parsed call arguments are recorded under a namespaced **`python`** attribute bag
on each step (the same shape n8n uses for its `n8n` provenance), e.g.

```json
{ "python": { "call": "train", "assigned_to": "m",
              "args": [ {"ref": "ds"} ], "kwargs": { "target": "outcome" } } }
```

A literal argument keeps its value; a variable / nested call / collection is
recorded as a reference marker (`{"ref": …}` / `{"call": …}`) so the bag stays
serializable without trying to capture runtime values.

This adds **no new JSON format**: the input is Python source text, and the only
JSON the frontend emits is the already-schema-validated IR JSON form (the
[`ir::from_json`](execution-graph.md) trust boundary, which the output
round-trips through and passes by construction). The `python` attribute bag rides
in the IR node's free-form `attributes`, namespaced so it never collides with the
gate's annotation keys.

## Verb → step-kind mapping

`python_step_kind(verb)` is a static table keyed on the bare function/method name.
It is the documented extension point as the use-case catalog grows; an unmapped
verb is a hard miss (`std::nullopt`) — `PythonFrontend` then defaults it to
`Clean`, but the lookup stays honest so the recognized vocabulary is testable.

| Stage | Verbs (R1) |
|---|---|
| `connect` | `load_dataset`, `load`, `read_csv`, `read_sql`, `read_json`, `read_parquet`, `read_table`, `connect`, `source`, `fetch`, `extract`, `open` |
| `clean` | `clean`, `dropna`, `fillna`, `normalize`, `dedupe`, `transform`, `featurize`, `features`, `prepare`, `preprocess`, `filter`, `select`, `join`, `merge`, `groupby`, `aggregate`, `encode`, `scale`, `impute` |
| `model` | `train`, `fit`, `evaluate`, `model`, `classify`, `cluster`, `forecast`, `calibrate` |
| `index` | `index`, `build_index`, `embed`, `vectorize`, `upsert`, `rank`, `score`, `predict` |
| `serve` | `serve`, `deploy`, `endpoint`, `expose`, `publish`, `explain` |
| `act` | `notify`, `alert`, `send`, `act`, `write`, `email`, `slack`, `webhook`, `trigger`, `export`, `save`, `add_nodes`, `add_node` |

The mapping is **engine-faithful**, not just lexical — each verb lowers to the
stage whose engine operation it names, so the lowered step runs the right work
(see [Engine bindings](#engine-bindings) below). In particular, training and
inference are **different stages**: `train` / `fit` / `calibrate` lower to `model`
(the native GBDT trainer the [`ChurnStepExecutor`](../use-cases/churn-step-executor.md) runs in
the model step), while applying a trained model — `score` / `predict` / `rank` —
lowers to `index` (the score job / at-risk list the executor runs in the index
step). `explain` lowers to `serve` (TreeSHAP — the churn-driver explain job the
executor runs in the serve step), and `add_nodes` lowers to `act` (provisioning is
a side effect on the cluster).

## Engine bindings

Lowering is only half the story — a verb has to **execute the right work**, not
just carry the right `StepKind` label (#522, "bind the sim-Python surface to the
engine"). The cluster + ML verbs bind to their engine operations through the
seams that already exist:

| Sim-Python | Lowers to | Executes as | Engine operation |
|---|---|---|---|
| `load_dataset("<path>", columns=…)` | `connect` | [`zetta::input::run_load_dataset_step`](datasets-shim.md) / `DatasetStepExecutor` | decode the Parquet file with the native [Parquet reader](../data/parquet-reader.md) → a (partitioned) table |
| `train(ds, target=…)` | `model` | [`ChurnStepExecutor`](../use-cases/churn-step-executor.md)`::run_model` | native distributed [GBDT train](../ml/gbdt.md) → a real [`Model`](../ml/models.md) |
| `rank(model)` / `score(…)` | `index` | `ChurnStepExecutor::run_index` | distributed [batch scoring / rank-at-risk](../ml/gbdt-score.md) |
| `explain(model)` | `serve` | `ChurnStepExecutor::run_serve` | exact [TreeSHAP](../ml/tree-shap.md) (per-entity + population drivers) |
| `cluster.add_nodes(n, provider=…)` | `act` | `zetta::input::run_add_nodes_step` | the permanent-add control action [`zetta::add_nodes`](../cloud/add-nodes.md) |

The three ML verbs need **no new execution code**: because they lower to the
`model` / `index` / `serve` steps the `ChurnStepExecutor` already interprets, a
lowered sim-Python churn script runs end-to-end through
[`run_churn_solution`](../use-cases/churn-solution.md) exactly like the catalog blueprint —
producing a real trained `Model`, the ranked at-risk list, and the churn drivers.

`cluster.add_nodes` is the one verb that is **not** a data-pipeline step — it is a
cluster control action. The frontend lowers it to an `act`-stage node (recording
`n` + `provider=` in the `python` provenance bag), and the matching execution
binding `zetta::input::run_add_nodes_step` (`include/zetta/input/cluster_bindings.hpp`)
reads the count from that node and drives `zetta::add_nodes`:

```cpp
zetta::input::FrontendResult lowered =
    zetta::input::PythonFrontend{}.compile("cluster.add_nodes(3, provider='azure')");
const auto* step = lowered.graph.find_node("add_nodes").get();
zetta::AddNodesResult added = zetta::input::run_add_nodes_step(*step, provisioner);
// added.members.size() == 3 (best-effort)
```

`load_dataset` is the **`datasets` library shim** (#521): the frontend already
lowers it to a `connect` node carrying the parquet path + a `columns=` projection,
and the binding [`zetta::input::run_load_dataset_step`](datasets-shim.md)
(`include/zetta/input/datasets_bindings.hpp`) reproduces the *meaning* of
`load_dataset` with zetta's own primitives — decode the referenced Parquet bytes
with the native [Parquet reader](../data/parquet-reader.md) and populate a (partitioned)
table. HuggingFace `datasets` is the fidelity reference only; file/network IO lives
behind an injectable `DatasetResolver` (offline default = a local path), so the
engine / build / CI never touch the network. The `DatasetStepExecutor` runs it inside
a `GraphExecutor`. See [datasets-shim.md](datasets-shim.md).

**Cloud stays consent-gated.** The `provider="azure"` argument names *which*
backend the author intended and rides as provenance only; `run_add_nodes_step`
does **not** construct a cloud provisioner from it. The `Provisioner&` is injected
by the host — the layer that owns the per-action [cloud consent gate](../../CLAUDE.md)
— so the binding only ever drives a provisioner it was handed (a fake in tests, a
real one a consenting caller already holds). And it adds **no new JSON format**:
the `python` provenance bag rides inside the IR node's free-form `attributes`,
already validated by the [IR JSON boundary](execution-graph.md), and the
[`AddNodesRequest`](../cloud/add-nodes.md) it drives has its own validated boundary.

## Scope / later slices

This frontend is the **parser + lowering**, plus the cluster control binding
above. Wiring it into a top-level input dispatch (so the GUI query box /
new-project import auto-detects Python vs SQL and routes to the right frontend) is
a later slice, as is growing the grammar (binary expressions, control flow as
control edges, richer literal capture). The produced IR already executes through
the existing [`GraphExecutor`](execution.md) / `ChurnStepExecutor` and passes the
[gate](gate.md) — no orchestrator changes are needed.

## Files

- `include/zetta/input/language_frontend.hpp` — the `LanguageFrontend` interface +
  `FrontendResult` (ENT-38 / ADR-33).
- `include/zetta/input/python_frontend.hpp` — `PythonFrontend` + `python_step_kind`.
- `src/core/input/python_frontend.cpp` — the non-throwing tokenizer +
  recursive-descent parser + the AST → IR lowering + the verb table.
- `include/zetta/input/cluster_bindings.hpp` /
  `src/core/input/cluster_bindings.cpp` — the cluster control binding:
  `run_add_nodes_step` (a lowered `cluster.add_nodes` IR node → `zetta::add_nodes`)
  + `add_nodes_count` (#522).
- `include/zetta/input/datasets_bindings.hpp` /
  `src/core/input/datasets_bindings.cpp` — the `datasets` shim binding:
  `dataset_load_spec` + `run_load_dataset_step` + `DatasetStepExecutor` (a lowered
  `load_dataset(...)` IR node → a (partitioned) table via the native Parquet reader),
  behind the injectable `DatasetResolver` IO seam (#521). See
  [datasets-shim.md](datasets-shim.md).
- `tests/test_python_frontend.cpp` — the lowering acceptance + the subset coverage.
- `tests/test_python_engine_bindings.cpp` — the #522 end-to-end acceptance: a
  lowered sim-Python churn script runs through the real engine, and a lowered
  `cluster.add_nodes` step drives `zetta::add_nodes`.

## See also

- [execution-graph.md](execution-graph.md) — the ExecutionGraph IR this lowers to.
- [datasets-shim.md](datasets-shim.md) — the `load_dataset(...)` engine binding
  (parquet path + column projection → a (partitioned) table; #521).
- [n8n-compile.md](n8n-compile.md) — the sibling compiler (n8n workflow → IR).
- [intent-compiler.md](intent-compiler.md) — NL intent → templated IR skeleton.
- [query.md](../data/query.md) — the `Dialect` interface (SQL text → the query AST), the
  pattern this frontend mirrors for the IR.
- the [`new-input`](../../.claude/skills/new-input/SKILL.md) skill — how to teach
  zetta a new kind of input.
