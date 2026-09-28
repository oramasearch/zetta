# Native out-of-fold ensembling and stacking

M-18 trains several native base models and blends their probabilities with a
regularized logistic regression meta-model. The trainer is deterministic under
its seed, uses no foreign ML runtime, and operates over each member's owned rows.

- Public API: [`include/zetta/ml/ensemble.hpp`](../../include/zetta/ml/ensemble.hpp)
- Distributed job: [`include/zetta/ml/ensemble_train_job.hpp`](../../include/zetta/ml/ensemble_train_job.hpp)
- Implementation: [`src/core/ml/ensemble.cpp`](../../src/core/ml/ensemble.cpp) and
  [`src/core/ml/ensemble_train_job.cpp`](../../src/core/ml/ensemble_train_job.cpp)
- Tests: [`tests/ml/test_ensemble.cpp`](../../tests/ml/test_ensemble.cpp) and
  [`tests/ml/test_ensemble_train_partitions.cpp`](../../tests/ml/test_ensemble_train_partitions.cpp)

## Inputs

`EnsembleTrainingData` contains an integer entity key, a binary label, named
numeric feature columns, and an optional numeric time column. `EnsembleParams`
sets the seed, external holdout fraction, fold count, regularization, optimizer
settings, and base-model specifications. Each base currently uses Zetta's native
histogram GBDT and may select its own feature subset and tree parameters.

The default pair uses shallow and deeper GBDTs over all features. Callers may
supply other GBDT variants through `EnsembleBaseSpec`. The trainer API keeps base
prediction columns separate from the meta-model, so another native methodology
can be adapted to this contract when it can produce deterministic probabilities
for fold training and full-data refitting.

## Correctness rules

The following rules are enforced by the implementation:

1. The stacker accepts only `StackingPredictionSource::OutOfFold`. Passing
   `InSample` is an error before optimization starts.
2. Every non-time training entity is assigned to exactly one deterministic fold
   by hashing its entity key with the seed. A base prediction for that entity is
   produced by a model trained without that fold.
3. Time-dependent inputs use expanding-window folds. A validation block is
   scored only by bases trained on earlier blocks, and the latest observations
   form the external holdout. The first block is warm-up data and does not fit
   the meta-model.
4. The meta-model is fit only from complete out-of-fold prediction rows. After
   it is fit, each base is refit over every non-holdout training row.
5. The external holdout is never used for base fitting or stacker fitting. It is
   used only for the final comparison with the best refit base model.

The time boundary is computed with cluster-wide minimum and maximum reductions,
so every member applies the same expanding-window plan. Inputs with fewer than
two folds, invalid holdout settings, missing features, ragged columns, or labels
without both classes are refused.

## Distribution

`run_ensemble_train_job` reads a device-backed local frame and runs one SPMD
training body per member. Base GBDT histogram statistics, meta-model gradients,
row counts, and time bounds use `JobRound::allreduce`. Only score/label pairs for
exact metrics are gathered and broadcast. Every member therefore receives the
same base models, stacker weights, and global metrics while retaining scores for
its owned rows.

## Outputs and metrics

`EnsembleTrainResult` returns:

- the refit base models and logistic meta-model intercept and weights;
- a blended probability plus each base probability for every local row;
- out-of-fold ROC-AUC, log loss, and accuracy;
- external holdout metrics for the stack and every base;
- the best single-model holdout ROC-AUC; and
- absolute ROC-AUC lift of the stack over that best base.

The serialized model body records the base model names, feature mappings,
native GBDT weights, and meta-model weights. No pretrained checkpoint,
tokenizer, vocabulary, or external configuration is used, so there are no
third-party weight licences or download-based tests for M-18.

## Running it

At the native layer, construct an `EnsembleTrainingData` or invoke
`run_ensemble_train_job` concurrently on every cluster member with the same
round tag. Registry compilation and persisted run outputs are documented in the
methodology section once the M-18 adapter is selected.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Ensembl*:*Stack*'
```

The focused tests prove that two complementary bases are improved by the stack,
that in-sample meta-model training is refused, that seeded runs are deterministic,
that time folds never use future observations, and that owned partitions converge
on one distributed stack.
