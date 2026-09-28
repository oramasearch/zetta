# ML docs

The native ML engine — the "learns from your data" differentiator, re-implemented from scratch as
distributed C++ over the [DDF](../ddf/ddf.md) + [compute](../compute/compute-device.md) seams (no
Python / XGBoost / foreign runtime, ADR-17). The M-1 reference pipeline trains a
histogram GBDT (allreduced histograms, SPMD) → calibrate → explain with exact TreeSHAP → score/rank
— each a recorded `ComputeJob`, producing a registered `Model`, proven correct by a native oracle
rather than a reference runtime. Read here for any model-training, explanation, or scoring work.

- [models.md](models.md) — the `Model` entity: replicated metadata + weights as a peer-to-peer large-object body + the `system.models` registry (ENT-14 / FEAT-6).
- [gbdt.md](gbdt.md) — distributed histogram GBDT training (binary logistic): per-node histograms allreduced each round, single-node == multi-node; produces a `Model` (`train_gbdt`, #390).
- [held-out-metrics.md](held-out-metrics.md) — hold rows OUT of training and measure the model on them: what `in_sample` vs `held_out` mean, the sharding-invariant split, and why both ride one collective (`holdout_fraction`, #953).
- [calibration.md](calibration.md) — probability calibration: Platt + isotonic fit on a held-out split, attached to the `GbdtModel` (Brier/ECE improve, AUC preserved) (`Calibrator`, #391).
- [gbdt-score.md](gbdt-score.md) — batch scoring / rank-at-risk: calibrated risk per entity, ranked into an at-risk list, SPMD with a device `sort_by_key` rank (`score_gbdt`, #392).
- [explaining-one-row.md](explaining-one-row.md) — "why is THIS one at risk": exact per-row TreeSHAP for one account, recomputed from the Model, contributions that sum to the score (`explain`, #948 step 6).
- [tree-shap.md](tree-shap.md) — exact path-dependent TreeSHAP over the trained trees: per-entity drivers + an allreduced global mean-|φ| ranking (`tree_shap`, #393).
- [native-oracle.md](native-oracle.md) — the native correctness oracle: committed goldens + analytic micro-tests + cross-cluster equivalence, no reference runtime (`load_golden_oracle`, RSK-8 / #395).
- [methodologies.md](methodologies.md) — the 21-entry methodology registry, compile resolution, run dispatch, and extension contract (#1061).
- [label-efficiency.md](label-efficiency.md) — M-19 active learning plus a native weak-supervision label model, with queryable label-queue and probabilistic-label outputs (#1058).

## Methodology portfolio

This table is generated from `zetta::ml::methodologies()`. Required input roles
carry `*`. Regenerate it with
`ZETTA_WRITE_METHODOLOGIES=1 ./build/tests/zetta-tests --gtest_filter=MethodologyRegistry.DocumentationTableMatchesTheRegistry`.

<!-- methodology-table:start -->
| ID | Methodology | Family | Model kinds | Solutions | Inputs | Outputs | Metrics | Status | Issue |
|---|---|---|---|---|---|---|---|---|---|
| M-1 | Tabular supervised learning | The tabular decision core | gbdt, gradient_boosting, classifier, classification, regression, score, tabular | S-1, S-2, S-3, S-4, S-10 | entity*, label*, time | model, scores, drivers, ranked entities | ROC-AUC, PR-AUC, log loss, Brier score, lift | runnable | — |
| M-2 | Entity resolution | Identity & structure | entity_resolution, entity-resolution | — | entity*, group | entity map, match evidence | pair precision, pair recall, blocking recall | not runnable | [#1041](https://github.com/oramasearch/zetta/issues/1041) |
| M-3 | Segmentation / clustering | Identity & structure | segmentation, clustering, cluster | S-9 | entity*, feature* | segment assignments, segment profiles | silhouette, stability, cluster size | not runnable | [#1042](https://github.com/oramasearch/zetta/issues/1042) |
| M-4 | Time-series forecasting | Time & vigilance | forecast, forecasting, time_series | S-5, S-6 | time*, label*, group | forecasts, prediction intervals | MAE, RMSE, WAPE, forecast value added | not runnable | [#1043](https://github.com/oramasearch/zetta/issues/1043) |
| M-5 | Survival / time-to-event | The tabular decision core | survival, time_to_event | S-8 | entity*, time*, label* | risk curves, expected time to event | concordance, time-dependent AUC, IPCW Brier | not runnable | [#1044](https://github.com/oramasearch/zetta/issues/1044) |
| M-6 | Causal inference & uplift | Action & decision | causal, uplift | — | entity*, treatment*, label* | treatment effects, targeting policy | uplift, Qini, policy value | not runnable | [#1045](https://github.com/oramasearch/zetta/issues/1045) |
| M-7 | Anomaly detection | Time & vigilance | anomaly, anomaly_detection, outlier | S-7 | time, feature* | anomaly scores, alerts | precision, recall, false alerts | not runnable | [#1046](https://github.com/oramasearch/zetta/issues/1046) |
| M-8 | Recommendation / next-best-action | Action & decision | recommendation, recommender, next_best_action | S-11 | entity*, treatment*, label | ranked actions, recommendations | NDCG, recall at k, policy value | not runnable | [#1047](https://github.com/oramasearch/zetta/issues/1047) |
| M-9 | Learning-to-rank | Action & decision | ranking, learning_to_rank, rank | S-12 | entity*, group, label* | ranked list, priority scores | NDCG, precision at k, queue yield | not runnable | [#1048](https://github.com/oramasearch/zetta/issues/1048) |
| M-10 | NLP / embeddings / RAG / fine-tuning | Language, documents & perception | nlp, embedding, embeddings, rag, fine_tuning, retrieval | — | text*, label | embeddings, search results, generated text | recall at k, MRR, F1, groundedness | partial | [#1049](https://github.com/oramasearch/zetta/issues/1049) |
| M-11 | Document intelligence | Language, documents & perception | document_intelligence, ocr, document | — | text*, label | structured fields, evidence links | field accuracy, F1, straight-through rate | not runnable | [#1050](https://github.com/oramasearch/zetta/issues/1050) |
| M-12 | Graph ML | Identity & structure | graph_ml, graph | — | entity*, group*, time | node scores, links, graph embeddings | average precision, AUC, lift | not runnable | [#1051](https://github.com/oramasearch/zetta/issues/1051) |
| M-13 | Computer vision | Language, documents & perception | computer_vision, vision | — | image*, label | classifications, detections, masks | mAP, IoU, escape rate | not runnable | [#1052](https://github.com/oramasearch/zetta/issues/1052) |
| M-14 | Speech & audio analytics | Language, documents & perception | speech, audio | — | audio*, label | transcripts, speaker segments, audio scores | word error rate, F1 | not runnable | [#1053](https://github.com/oramasearch/zetta/issues/1053) |
| M-15 | Bandits & sequential decisioning | Action & decision | bandit, bandits, sequential_decisioning | — | entity*, treatment*, label* | action policy, propensities | cumulative reward, regret, policy value | not runnable | [#1054](https://github.com/oramasearch/zetta/issues/1054) |
| M-16 | Optimization / operations research | Action & decision | optimization, operations_research | — | feature*, group* | plan, allocation, constraint trace | objective value, constraint violations | not runnable | [#1055](https://github.com/oramasearch/zetta/issues/1055) |
| M-17 | Bayesian / probabilistic modeling & uncertainty quantification | The tabular decision core | bayesian, probabilistic, uncertainty | — | feature*, group | posterior, prediction intervals | coverage, CRPS, calibration | not runnable | [#1056](https://github.com/oramasearch/zetta/issues/1056) |
| M-18 | Ensembling & stacking | The tabular decision core | ensemble, ensembling, stacking | — | feature*, label* | ensemble model, blended predictions | delta over best model, stability | not runnable | [#1057](https://github.com/oramasearch/zetta/issues/1057) |
| M-19 | Label efficiency | The data supply chain | label_efficiency, active_learning, weak_supervision | label-efficient-training, support-ticket-triage | entity*, feature*, label* | label queue table, weak-label table | accuracy at labelling budget, labels saved, label-model gold accuracy | runnable | — |
| M-20 | Privacy-preserving learning & synthetic data | The data supply chain | privacy_preserving, federated, synthetic_data | — | group*, feature* | private model, synthetic dataset, guarantee report | privacy budget, TSTR, attack success | not runnable | [#1059](https://github.com/oramasearch/zetta/issues/1059) |
| M-21 | Online / streaming learning | Time & vigilance | online_learning, streaming | — | time*, feature*, label | online model, streaming scores | prequential loss, drift, latency | not runnable | [#1060](https://github.com/oramasearch/zetta/issues/1060) |
<!-- methodology-table:end -->
