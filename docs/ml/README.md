# ML docs

The native ML engine — the "learns from your data" differentiator, re-implemented from scratch as
distributed C++ over the [DDF](../ddf/ddf.md) + [compute](../compute/compute-device.md) interfaces (no
Python / XGBoost / foreign runtime, ADR-17). The M-1 reference pipeline trains a
histogram GBDT (allreduced histograms, SPMD) → calibrate → explain with exact TreeSHAP → score/rank
— each a recorded `ComputeJob`, producing a registered `Model`, proven correct by a native oracle
rather than a reference runtime. M-3 segmentation compiles its own customer-table graph,
runs distributed k-means and reports entity assignments, descriptive profiles and separation
metrics through the methodology registry. Forecasting uses temporal backtests and prediction
intervals without probability calibration; M-4 is runnable on its pipeline's table. M-12 adds time-safe graph features and
native node embeddings over partitioned relationship edges. Read here
for model work.

- [segmentation.md](segmentation.md) — runnable M-3 / S-9: native distributed k-means, global preprocessing, graph checks, table outputs and run reports (#1042).

- [models.md](models.md) — the `Model` entity: replicated metadata + weights as a peer-to-peer large-object body + the `system.models` registry (ENT-14 / FEAT-6).
- [gbdt.md](gbdt.md) — distributed histogram GBDT training (binary logistic and squared error): per-node histograms allreduced each round, single-node == multi-node; produces a `Model` (`train_gbdt`, #390).
- [anomaly.md](anomaly.md) — runnable M-7/S-7: label-free distributed isolation forest + robust deviations and causal KPI residual scoring; exact alert budgets, correctness gates, table lineage, and registry run reports (#1046).
- [forecast.md](forecast.md) — runnable M-4: native multi-series forecasting, temporal backtests, seasonal-naive FVA, intervals, distributed table execution and report read-back (`forecast`, #1043).
- [survival.md](survival.md) — M-5 discrete-time survival: person-period expansion, distributed hazard GBDT, curves, censoring-aware metrics, and result tables (#1044).
- [uplift.md](uplift.md) — runnable M-6 causal uplift: native T- and X-learners, propensity overlap enforcement, held-out Qini/AUUC selection, effect and Qini tables, and registry execution (#1045).
- [graph.md](graph.md) — M-12 graph ML: time-bounded degrees, PageRank, components, label propagation, clustering coefficient, and node2vec-style embeddings; runnable through the methodology registry (#1051).
- [held-out-metrics.md](held-out-metrics.md) — hold rows OUT of training and measure the model on them: what `in_sample` vs `held_out` mean, the sharding-invariant split, and why both ride one collective (`holdout_fraction`, #953).
- [calibration.md](calibration.md) — probability calibration: Platt + isotonic fit on a held-out split, attached to the `GbdtModel` (Brier/ECE improve, AUC preserved) (`Calibrator`, #391).
- [gbdt-score.md](gbdt-score.md) — batch scoring / rank-at-risk: calibrated risk per entity, ranked into an at-risk list, SPMD with a device `sort_by_key` rank (`score_gbdt`, #392).
- [explaining-one-row.md](explaining-one-row.md) — "why is THIS one at risk": exact per-row TreeSHAP for one account, recomputed from the Model, contributions that sum to the score (`explain`, #948 step 6).
- [tree-shap.md](tree-shap.md) — exact path-dependent TreeSHAP over the trained trees: per-entity drivers + an allreduced global mean-|φ| ranking (`tree_shap`, #393).
- [native-oracle.md](native-oracle.md) — the native correctness oracle: committed goldens + analytic micro-tests + cross-cluster equivalence, no reference runtime (`load_golden_oracle`, RSK-8 / #395).
- [integration.md](integration.md) — integration branch merge order, primary solution ownership, diff audit and native proof (#1082).
- [methodologies.md](methodologies.md) — the 21-entry methodology registry, compile resolution, run dispatch, and extension contract (#1061).
- [Data-first featurization](../data/featurization.md) — M-1–M-16 run preconditions and native feature/exclusion provenance, including speech, bandit context, and solver inputs (#1062).
- [entity_resolution.md](entity_resolution.md) — M-2 native multi-route blocking, probabilistic or labelled-pair scoring, conflict-safe clustering, result tables, and metrics (#1041).
- [next_best_action.md](next_best_action.md) — M-8 propensity composition, implicit-feedback ALS, hard policy constraints, recommendation tables, and offline IPS/DR evaluation (#1047).
- [rank.md](rank.md) — M-9 native LambdaMART: whole-group validation, distributed histogram training, ranked-queue tables, NDCG@k, and yield at budget (#1048).
- [vision.md](vision.md) — M-13 native PNG/JPEG decoding, frozen ViT embeddings, source-disjoint distributed head training, prediction tables, and evaluation reports (#1052).
- [document_extraction.md](document_extraction.md) — M-11 native PDF text recovery and TrOCR line recognition, declared-field extraction, evidence retention, review routing, and field-level metrics (#1050).
- [language.md](language.md) — M-10 native WordPiece/BERT embeddings, retrieval, grounded answers, classifier tuning, metrics, and governance rules (#1049).
- [speech.md](speech.md) — M-14 native WAV transcription, Whisper tiny/base checkpoints, consent and privacy rules, transcript tables, and word error rate (#1053).
- [bandit.md](bandit.md) — M-15 native Thompson sampling, LinUCB, and linear Thompson sampling with allreduced per-arm state, constrained propensities, persisted policy tables, and registry execution (#1054).
- [optimization.md](optimization.md) — M-16 native revised-simplex LP and best-bound branch-and-bound MIP: scenario table in, independently verified plan and constraint trace out (#1055).

## Methodology portfolio

This table is generated from `zetta::ml::methodologies()`. Required input roles
carry `*`. Regenerate it with
`ZETTA_WRITE_METHODOLOGIES=1 ./build/tests/zetta-tests --gtest_filter=MethodologyRegistry.DocumentationTableMatchesTheRegistry`.

<!-- methodology-table:start -->
| ID | Methodology | Family | Model kinds | Solutions | Inputs | Outputs | Metrics | Status | Issue |
|---|---|---|---|---|---|---|---|---|---|
| M-1 | Tabular supervised learning | The tabular decision core | gbdt, gradient_boosting, classifier, classification, regression, score, tabular | S-1, S-2, S-3, S-4, S-10, S-12 | entity*, label*, time | model, scores, drivers, ranked entities | ROC-AUC, PR-AUC, log loss, Brier score, lift | runnable | — |
| M-2 | Entity resolution | Identity & structure | entity_resolution, entity-resolution | — | entity*, group*, label, time | resolved entities, match pairs, review band | pair precision, pair recall, B-cubed precision, B-cubed recall, blocking recall, merge rate, review-band size | runnable | — |
| M-3 | Segmentation / clustering | Identity & structure | segmentation, clustering, cluster | S-9 | entity*, feature*, label, time | model, segment assignments, segment profiles, run report | chosen k, silhouette, Davies-Bouldin, stability (ARI), inertia per k, segment size/share | runnable | [#1042](https://github.com/oramasearch/zetta/issues/1042) |
| M-4 | Time-series forecasting | Time & vigilance | forecast, forecasting, time_series | S-5, S-6 | time*, label*, group | forecast table with prediction intervals, forecast model | MAE, MASE, sMAPE, seasonal-naive MAE, forecast value added, interval coverage | runnable | [#1043](https://github.com/oramasearch/zetta/issues/1043) |
| M-5 | Survival / time-to-event | The tabular decision core | survival, time_to_event | — | entity*, time*, label*, feature_time*, feature* | survival table, interval hazards, expected time to event, median survival, interval calibration | censoring-aware concordance, IPCW integrated Brier score | runnable | — |
| M-6 | Causal inference & uplift | Action & decision | causal, uplift | — | entity*, treatment*, label*, time, feature* | uplift model, treatment effects, targeting policy, Qini curve, run report | held-out AUUC, Qini coefficient, uplift at targeting fraction | runnable | [#1045](https://github.com/oramasearch/zetta/issues/1045) |
| M-7 | Anomaly detection | Time & vigilance | anomaly, anomaly_detection, outlier | S-7, S-8 | feature*, entity, label, time | anomaly table, budget report, model, lineage | threshold, contamination, alert_budget, alert_rate, precision_at_k | runnable | [#1046](https://github.com/oramasearch/zetta/issues/1046) |
| M-8 | Recommendation / next-best-action | Action & decision | next_best_action, recommendation, recommender | S-11 | entity*, treatment*, label*, time*, feature*, group | recommendation table, policy report, next-best-action model | hit rate at k, popularity hit rate at k, inverse propensity value, doubly robust value, effective sample size, maximum clipped weight | runnable | [#1047](https://github.com/oramasearch/zetta/issues/1047) |
| M-9 | Learning-to-rank | Action & decision | ranking, learning_to_rank, rank | — | entity*, group*, label*, time, feature* | ranked-queue table, priority scores, model | NDCG@k, yield at budget | runnable | [#1048](https://github.com/oramasearch/zetta/issues/1048) |
| M-10 | NLP / embeddings / RAG / fine-tuning | Language, documents & perception | language, nlp, embedding, embeddings, rag, fine_tuning, retrieval | — | entity*, text*, label*, group | embeddings table, retrieval table, grounded answers table, classifier head | recall at k, MRR, groundedness | runnable | [#1049](https://github.com/oramasearch/zetta/issues/1049) |
| M-11 | Document intelligence | Language, documents & perception | document_extraction, document_intelligence, ocr, document | document_automation | document*, label | extracted fields, review queue, evidence links | field precision, field recall, field F1, review share | runnable | [#1050](https://github.com/oramasearch/zetta/issues/1050) |
| M-12 | Graph ML | Identity & structure | graph, graph_ml | — | entity*, group*, feature*, time*, label* | node feature table, graph summary, node scores, graph embeddings | held-out node accuracy, component count, degree distribution, PageRank top nodes | runnable | [#1051](https://github.com/oramasearch/zetta/issues/1051) |
| M-13 | Computer vision | Language, documents & perception | computer_vision, vision | — | image*, label*, group* | vision model, prediction table, confusion matrix | accuracy, macro-F1 | runnable | — |
| M-14 | Speech & audio analytics | Language, documents & perception | speech, audio | — | audio*, group*, label | transcript table, speech model | word error rate | runnable | [#1053](https://github.com/oramasearch/zetta/issues/1053) |
| M-15 | Bandits & sequential decisioning | Action & decision | bandit, bandits, sequential_decisioning | — | entity*, treatment*, label*, feature | allocation policy, decision propensities | cumulative reward, regret, exploration share | runnable | — |
| M-16 | Optimization / operations research | Action & decision | optimization, operations_research | — | variable*, objective*, constraint*, scenario*, group | plan, constraint trace, solver status | objective value, current plan objective, objective delta, constraint violations, optimality gap | runnable | [#1055](https://github.com/oramasearch/zetta/issues/1055) |
| M-17 | Bayesian / probabilistic modeling & uncertainty quantification | The tabular decision core | bayesian, probabilistic, uncertainty | — | feature*, group | posterior, prediction intervals | coverage, CRPS, calibration | not runnable | [#1056](https://github.com/oramasearch/zetta/issues/1056) |
| M-18 | Ensembling & stacking | The tabular decision core | ensemble, ensembling, stacking | — | feature*, label* | ensemble model, blended predictions | delta over best model, stability | not runnable | [#1057](https://github.com/oramasearch/zetta/issues/1057) |
| M-19 | Label efficiency | The data supply chain | label_efficiency, active_learning, weak_supervision | — | feature*, label | labels, review queue, label provenance | agreement, cost per label, downstream lift | not runnable | [#1058](https://github.com/oramasearch/zetta/issues/1058) |
| M-20 | Privacy-preserving learning & synthetic data | The data supply chain | privacy_preserving, federated, synthetic_data | — | group*, feature* | private model, synthetic dataset, guarantee report | privacy budget, TSTR, attack success | not runnable | [#1059](https://github.com/oramasearch/zetta/issues/1059) |
| M-21 | Online / streaming learning | Time & vigilance | online_learning, streaming | — | time*, feature*, label | online model, streaming scores | prequential loss, drift, latency | not runnable | [#1060](https://github.com/oramasearch/zetta/issues/1060) |
<!-- methodology-table:end -->
