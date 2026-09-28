# Optimization / operations research (M-16)

Implemented for [#1055](https://github.com/oramasearch/zetta/issues/1055), in
the methodology portfolio tracked by
[#984](https://github.com/oramasearch/zetta/issues/984).

M-16 turns scenario-aware predictions and operating constraints into a decision
plan. It solves linear programs with a native two-phase revised simplex and
mixed-integer linear programs with native best-bound branch-and-bound. The
registered `model_kind` is `optimization`; `operations_research` is an alias.
No Python runtime, hosted service, or external solver participates.

S-4 remains owned by M-1: the catalogue's collections-prioritization row lists
tabular supervised learning first. M-16 remains selectable by methodology ID
and its model kinds; the registry has no separate supporting-component field.

## Solver

`zetta::ml::solve_optimization` accepts `OptimizationProblem` and returns an
`OptimizationResult`:

- LP variables have lower and optional upper bounds. Lower bounds are shifted
  into standard form and finite upper bounds become explicit rows.
- The LP solver uses a phase-one artificial-variable objective, a revised
  simplex basis, partial-pivot basis refactorization, and Bland selection for
  entering variables and ratio-test ties.
- Integer and binary variables use best-bound branch-and-bound. Each child adds
  a bound and solves its LP relaxation. `node_limit` and `time_limit_ms` bound
  the search; a feasible incumbent carries a relative optimality gap.
- The supported CP-SAT form is the linear integer/binary subset: boolean and
  bounded integer decisions with linear equality or inequality constraints.
  General global constraints, nonlinear expressions, and interval scheduling
  are not accepted as if they were linear.
- `seed` is recorded for reproducibility. The current Bland and best-bound
  ordering is deterministic and does not consume randomness.

Every candidate plan is checked again against the original variable bounds,
integrality requirements, and every original constraint. This verifier does
not read simplex or branch bookkeeping. A candidate that fails verification is
discarded and reported as `numerical_failure`.

## Input table

One row contributes one variable coefficient to one constraint. A variable may
appear in several rows. Repeated variable or constraint metadata must agree.

| Column | Required | Meaning |
|---|---:|---|
| `row_id` | yes | Unique integer row key used by the shared resolved-dataset contract. |
| `variable` | yes | Stable decision-variable name. |
| `objective` | yes | Cost or value coefficient. |
| `lower_bound` | no | Lower bound; defaults to `0`. |
| `upper_bound` | no | Upper bound; omitted means no finite upper bound. |
| `integer` | yes | Whether the variable is integral. Boolean variables use bounds 0 and 1. |
| `current_value` | no | Current-plan decision, used for objective comparison; defaults to `0`. |
| `scenario` or `quantile` | yes | Scenario name or quantile in `[0,1]`. A row marked `prediction_kind: point` is refused. |
| `constraint` | no | Constraint name. Omit only for a variable-declaration row. |
| `coefficient` | with constraint | This variable's coefficient in the named constraint. |
| `sense` | with constraint | `<=`, `=`, or `>=`. |
| `rhs` | with constraint | Right-hand side. |

`integer` also serves as the shared dataset contract's binary label column and
`objective` as its numeric feature column. M-16 does not train a classifier;
these bindings let the existing `ResolvedDataset` boundary validate and locate
the same live table without introducing a second dataset-address grammar.

Prediction inputs must be scenarios or quantiles, never an unqualified point
forecast. Constraint and master-data values that are deterministic use a named
scenario such as `base` or `deterministic`.

## Outputs and report

A feasible run writes two replicated tables under `optimization_results`:

- `plan_<run-id>`: `variable`, `decision`, `current_value`, objective
  coefficient, and integer marker.
- `constraints_<run-id>`: constraint activity, right-hand side, violation, and
  `satisfied` for every constraint.

The run also registers a model-kind `optimization` result record and a report
output. `get_run_report` returns:

- solver status;
- objective value, current-plan objective, and their delta;
- feasibility and constraint-violation count;
- MIP optimality gap, explored nodes, and simplex iterations;
- the plan and constraint rows; and
- the source table and input-row count.

`infeasible`, `unbounded`, and a node/time/iteration limit reached without an
incumbent are recorded solver conclusions with no plan table. A node/time limit
may return a verified incumbent and its nonzero gap. Status and gap are always
recorded. An invalid model or numerically rejected candidate fails the run.

## Distribution

The registry runner reads a partition-owned input table through the cluster-aware
dataset sampler, so every owner contributes its rows and a missing partition
fails the read. Basis pivots and branch-queue choices are ordered global state;
the current reference solver performs those decisions on the coordinator.

`OptimizationAllReduce` is the cluster callback for independent post-solve
statistics. A co-launched caller can reduce maximum violation and violation
count before publishing a plan. The registry path currently verifies the
coordinator's complete gathered matrix and does not install a collective. The
callback keeps distributed verification available through the public API while
the ordered basis and branch queue remain coordinator-owned.

## Correctness rules

The default correctness checks refuse an M-16 graph unless its model step says:

- `scenario_input: true`;
- `verify_constraints: true`; and
- `report_solver_status: true`.

The table parser then refuses point forecasts and inconsistent metadata. After
solving, the independent verifier enforces all bounds, integrality conditions,
and constraints. No plan is published if any member reports a violation.

## Running it

1. Land the JSONL/CSV table with `upload_dataset`, `add_dataset`, or a source
   query.
2. Create a project and capture the optimization intent.
3. Call `compile_intent` with `methodology: "M-16"` and hints naming the table:

   ```json
   {"source":"operations","table":"decisions"}
   ```

4. Call `run` with the compiled graph. Omitting `dataset_path` reads that
   graph's connect-step table. Optional `optimization` controls are `maximize`,
   `iteration_limit`, `node_limit`, `time_limit_ms`, tolerances, and `seed`.
5. Read the durable run with `get_run`, then call `get_run_report`.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Simplex*:*Optimiz*:*BranchBound*:*Mip*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:*GraphCheck*:ActionJourneys.*:Uc1Journey.*'
./build/tests/zetta-tests
```

The focused tests cover textbook optima, degeneracy, infeasibility,
unboundedness, bounded minimization, a knapsack MIP, limit/gap reporting,
point-forecast refusal, independent and collective violation detection,
registry compilation, pipeline-table reading, output tables, and report facts.

## Weights and licences

M-16 uses no pretrained weights, tokenizer, vocabulary, or model configuration.
There is therefore no checkpoint revision or weights licence to record.
