# ML docs

The native ML engine — the "learns from your data" differentiator, re-implemented from scratch as
distributed C++ over the [DDF](../ddf/ddf.md) + [compute](../compute/compute-device.md) seams (no
Python / XGBoost / foreign runtime, ADR-17). The pipeline shape every use case reuses: train a
histogram GBDT (allreduced histograms, SPMD) → calibrate → explain with exact TreeSHAP → score/rank
— each a recorded `ComputeJob`, producing a registered `Model`, proven correct by a native oracle
rather than a reference runtime. Read here for any model-training, explanation, or scoring work.

- [models.md](models.md) — the `Model` entity: replicated metadata + weights as a peer-to-peer large-object body + the `system.models` registry (ENT-14 / FEAT-6).
- [gbdt.md](gbdt.md) — distributed histogram GBDT training (binary logistic): per-node histograms allreduced each round, single-node == multi-node; produces a `Model` (`train_gbdt`, #390).
- [held-out-metrics.md](held-out-metrics.md) — hold rows OUT of training and measure the model on them: what `in_sample` vs `held_out` mean, the sharding-invariant split, and why both ride one collective (`holdout_fraction`, #953).
- [calibration.md](calibration.md) — probability calibration: Platt + isotonic fit on a held-out split, attached to the `GbdtModel` (Brier/ECE improve, AUC preserved) (`Calibrator`, #391).
- [gbdt-score.md](gbdt-score.md) — batch scoring / rank-at-risk: calibrated risk per entity, ranked into an at-risk list, SPMD with a device `sort_by_key` rank (`score_gbdt`, #392).
- [tree-shap.md](tree-shap.md) — exact path-dependent TreeSHAP over the trained trees: per-entity drivers + an allreduced global mean-|φ| ranking (`tree_shap`, #393).
- [native-oracle.md](native-oracle.md) — the native correctness oracle: committed goldens + analytic micro-tests + cross-cluster equivalence, no reference runtime (`load_golden_oracle`, RSK-8 / #395).
