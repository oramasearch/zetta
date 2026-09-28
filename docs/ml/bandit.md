# Bandits and sequential decisioning

M-15 implements native optimize-while-learning policies for
[#1054](https://github.com/oramasearch/zetta/issues/1054), under the methodology
epic [#984](https://github.com/oramasearch/zetta/issues/984). The registered
`model_kind` is `bandit`.

On the integration branch, S-11 resolves to M-8, the catalogue's primary
recommendation / next-best-action method. M-15 is a supporting policy component;
select it by `M-15`, `bandit`, `bandits`, or `sequential_decisioning`.

## Algorithms

- Context-free arms use Beta-Bernoulli Thompson sampling. Each arm persists its
  success and failure posterior parameters.
- Contextual arms use per-arm ridge statistics `A = lambda I + sum(xx^T)` and
  `b = sum(rx)`. LinUCB chooses from the posterior mean plus its confidence
  radius. Linear Thompson sampling draws from the full `A^-1` covariance.
- A fixed seed makes sampling, policy probabilities, and chosen arms
  reproducible.

The distributed job receives only the observations owned by its member. Every
member contributes per-arm counts or `(A, b)` deltas through
`JobRound::allreduce`; raw decisions do not move between members. Every member
therefore obtains the same updated policy, while its decision log contains the
local decisions it owns. Tests compare the distributed state with the
single-member result.

No pretrained weights, tokenizer, external model configuration, or model
licence is involved.

## Inputs

A decision row contains:

- a decision identifier;
- an arm or treatment;
- a reward in `[0, 1]`; and
- for LinUCB and linear Thompson sampling, an ordered numeric context vector.

The runner parameters name the allowed arms, baseline arm, algorithm, seed,
ridge/confidence values, exploration floor, and caps. An arm not declared in
the parameters is rejected. The fixed baseline must name one of those arms.

## Outputs and persisted state

The policy output has one row per arm and context bucket:

- `arm`;
- `context_bucket`;
- `probability`;
- `expected_reward`; and
- the serialized sufficient statistics used to resume learning.

The decision output has one row per decision with the chosen arm, context
bucket, expected reward, and `propensity`. Propensities are the constrained
probabilities actually sampled, not scores reconstructed later. They therefore
support M-8 offline policy evaluation.

The run reports cumulative observed reward, estimated reward for the fixed
baseline arm, regret against that baseline, decision count, and exploration
share. The policy table is the restart boundary: a later run restores its
sufficient statistics before adding new observations.

## Correctness rules

1. Every arm receives a strictly positive exploration floor.
2. Every arm has an effective cap: the graph's default cap applies unless an
   arm-specific cap overrides it.
3. Floors and caps must be jointly feasible. A floor above an arm's cap, floors
   summing above one, or caps summing below one rejects the run.
4. Every decision records the chosen arm's propensity, and all arm
   probabilities sum to one.
5. Contextual statistics are per arm and have the declared context dimension.
6. Rewards are finite values in `[0, 1]`.

The M-15 correctness check enforces rules 1-2 on every compiled M-15 pipeline
and rule 3 when the graph declares its arms. The policy engine enforces all six
after run-time arms are resolved.

## Running it

Create a table with decision, arm, reward, and optional numeric context columns.
Create a project, capture its intent, compile with `methodology: "M-15"`, and
run the resulting pipeline with that table as `dataset_path`. Optional run
parameters choose `thompson`, `linucb`, or `linear_thompson`, set the seed,
and override the graph's floor and caps. Compile hints may bind
`decision_column`, `arm_column`, and `reward_column`; their defaults remain
`decision_id`, `arm`, and `reward`. Read the completed run with
`get_run_report`; its outputs name the policy and decision tables.

When `dataset_path` is omitted, the M-15 registry adapter resolves the source
and table on its connect step. That table adapter executes on the receiving
node. The public `run_bandit_job` path is the distributed primitive: callers
provide each member's owned observations and it allreduces only per-arm
sufficient statistics. Cross-member launch of the registry table adapter is
not part of this change.

Decision and reward delivery over pub/sub is deliberately outside this change;
this implementation consumes tables. The lake opportunity-to-project path is
also outside this repository slice.

## Feature-plan precondition

The registry callback checks the [feature plan](../data/featurization.md) before
policy construction. Its native entries record per-arm posterior statistics and
the effective numeric context list, whether explicitly supplied or inferred for
LinUCB/linear Thompson. Context-free Thompson has no inferred context features.
Reward and decision ID are excluded as predictors; selected leakage suspects are
refused through the shared check. A required data transform returns `invalid_input`
before any Run exists. `get_run_report.features` preserves used/excluded reasons.
Fallback planning retains declared input roles, so a reserved reward/key requested
as context is diagnosed as a role conflict, not as a missing label.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Bandit*:*Thompson*:*LinUCB*'
```

The tests simulate a known best arm and require concentration on it with
declining average regret, assert probability normalization and propensity
logging, stop and reopen a node to restore its policy table, exercise both
contextual algorithms, and compare several members after the allreduce rounds.
