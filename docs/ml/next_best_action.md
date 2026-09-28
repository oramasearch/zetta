# Next-best-action (`next_best_action`)

M-8 ranks candidate actions for each entity, then applies a policy that may choose at most one
action per entity. It combines two native models:

- one binary-logistic GBDT per action estimates response or conversion propensity from entity
  features;
- implicit-feedback alternating least squares (ALS) estimates entity-action affinity from
  interaction counts. Unobserved pairs remain weak negatives through the shared Gram term.

The ranking score is expected value (`propensity * action_value - action_cost`) plus the configured
ALS affinity weight. Propensity is predictive, not causal lift. Use M-6 causal inference and uplift
to decide whether an action should be applied at all.

## Input contract

`run_next_best_action_on_table` accepts a resolved dataset from
`zetta::data::resolve_dataset`. The resolved contract supplies the entity, response label, and
numeric feature columns. Entity and action IDs must be dense, non-negative integers. The table also
contains these configurable numeric columns:

| Role | Default column | Meaning |
|---|---|---|
| action | `action_id` | candidate item or action ID |
| action value | `action_value` | value received after response |
| action cost | `action_cost` | cost charged when selected |
| eligibility | `eligible` | value greater than 0.5 permits the action |
| contacts | `contacts` | contacts already made for the entity-action pair |
| contact cap | `contact_cap` | maximum permitted contacts |
| interaction count | `interaction_count` | implicit positive strength; zero is unobserved |
| interaction time | `interaction_time` | chronological ordering for the ALS holdout |
| logging propensity | `logging_propensity` | probability used by the historical logging policy |

The logging-propensity column can be omitted only when offline policy evaluation is not required.
The implementation never estimates or guesses a missing logging propensity.

## Training and distribution

Each participant reads only the table partitions it owns. Per-action GBDT histograms, ALS normal
equations, the chronological holdout, candidate aggregates, and policy-evaluation totals are
combined with `JobRound::allreduce`. All participants use the same seeded initialization and return
the same global rankings. The collective boundary carries only finite numeric payloads.

For ALS evaluation, the latest positive interaction for each entity is held out. Earlier positive
interactions fit the model. The split is based on the global maximum interaction time, so changing
the physical partitioning cannot turn a future event into training data.

## Policy and correctness rules

Scoring and policy enforcement are separate. After scoring, the policy:

1. rejects every ineligible or contact-capped candidate;
2. selects the best remaining candidate for each entity;
3. considers those entity choices from highest to lowest policy score and selects only choices that
   fit the global budget.

Eligibility and contact caps are hard constraints. They cannot be traded for a higher score. The
recommendation rows retain rejected candidates and state why they were not chosen.

Offline evaluation is refused unless every logged decision has a finite positive logging
propensity. When available, importance weights use the configured propensity floor and maximum
weight. The report records inverse propensity scoring (IPS), doubly robust (DR) value, effective
sample size, and the largest clipped weight.

## Outputs

The table runner writes two tables in `recommendation_results` and registers a Model with kind
`next_best_action`:

- `recommendations_<run>`: entity, action, rank, propensity, ALS affinity, expected value, policy
  score, cost, eligibility/cap state, chosen flag, and a human-readable reason;
- `policy_report_<run>`: row and action counts, hit rate@k, popularity hit rate@k, chronological
  holdout facts, IPS/DR values or the explicit refusal, effective sample size, clipped weight, and
  the causal warning.

The Run records both tables and the model as outputs and records the source table in lineage.

## Metrics

- `hit_rate_at_k`: ALS hits on the per-entity latest-interaction holdout;
- `popularity_hit_rate_at_k`: a popularity baseline over the same holdout;
- `inverse_propensity_value` and `doubly_robust_value`: offline policy estimates when logging
  propensities exist;
- `effective_sample_size` and `maximum_clipped_weight`: diagnostics for the weighted estimate.

## Running it

Phase A exposes the resolved-table runner in `zetta/ml/next_best_action_table.hpp`. Registry-driven
compile and run are described here when M-8 is connected to the methodology table.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*NextBest*:*Recommend*:*ALS*:*Policy*'
```

The focused tests recover a planted ALS affinity above popularity, enforce eligibility and contact
caps, recover a known synthetic policy value with IPS and DR, refuse evaluation without logging
propensities, and compare a partitioned run with the single-member result.
