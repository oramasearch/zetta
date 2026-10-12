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
for model work. M-21 adds event-time online logistic regression over pub/sub topics.

- [segmentation.md](segmentation.md) — runnable M-3 / S-9: native distributed k-means, ISO-or-epoch cutoff normalization, global preprocessing, graph checks, table outputs and run reports (#1042, #1199).

- [models.md](models.md) — the `Model` entity: replicated metadata with producing Run and Project/Pipeline names, reversible text entity-key maps, page-ready listing, peer-to-peer weights body, and the `system.models` registry (ENT-14 / FEAT-6, #957, #1184).
- [run-metrics.md](run-metrics.md) — replicated series, automatic per-metric steps, coalesced trainer captures, summaries, chart data, live topics, and DP suppression (#1137, #1205, #1217).
- [studies.md](studies.md) — the replicated background Study driver, published methodology search spaces, full-table split preflight and training, cost reservations, real M-1 dataset/Pipeline trial Runs, restart recovery, curves and pruning, analysis, promotion, live events, REST/MCP parity, the shared run feature plan, canonical metric names, failed-when-every-trial-failed, `create_study` error paths with the `from_defaults` offer, coalesced per-round captures, and the tables a Study reads (`reads`, zetta#1258) (#1138, #1205, #1207, #1209, #1216, #1217).
- [gbdt.md](gbdt.md) — distributed histogram GBDT training (binary logistic and squared error): per-node histograms allreduced each round, single-node == multi-node; produces a `Model` (`train_gbdt`, #390).
- [anomaly.md](anomaly.md) — runnable M-7/S-7: label-free distributed isolation forest + robust deviations and causal KPI residual scoring; exact alert budgets, correctness gates, table lineage, and registry run reports (#1046).
- [forecast.md](forecast.md) — runnable M-4: native multi-series forecasting, temporal backtests, seasonal-naive FVA, intervals, distributed table execution and report read-back (`forecast`, #1043).
- [survival.md](survival.md) — M-5 discrete-time survival: person-period expansion, distributed hazard GBDT, curves, censoring-aware metrics, and result tables (#1044).
- [uplift.md](uplift.md) — runnable M-6 causal uplift: native T- and X-learners, propensity overlap enforcement, held-out Qini/AUUC selection, effect and Qini tables, and registry execution (#1045).
- [graph.md](graph.md) — M-12 graph ML: time-bounded degrees, PageRank, components, label propagation, clustering coefficient, and node2vec-style embeddings; runnable through the methodology registry (#1051).
- [ensemble.md](ensemble.md) — M-18 native out-of-fold stacking: complementary GBDT variants, regularized logistic meta-model, time-ordered folds, refit bases, blended scores, and holdout lift over the best base (#1057).
- [held-out-metrics.md](held-out-metrics.md) — hold rows OUT of training and measure the model on them: content-hash splits for ordinary runs, out-of-time Study validation, why both metric sets ride one collective, and the canonical metric names with their accepted aliases, and the held-out caveat checks the run report and `model_health` share (`holdout_fraction`, #953, #1205, #1217, #1224).
- [calibration.md](calibration.md) — probability calibration: Platt + isotonic fit on a held-out split, attached to the `GbdtModel` (Brier/ECE improve, AUC preserved) (`Calibrator`, #391).
- [gbdt-score.md](gbdt-score.md) — batch scoring / rank-at-risk: calibrated risk per entity, ranked into an at-risk list, SPMD with a device `sort_by_key` rank (`score_gbdt`, #392).
- [explaining-one-row.md](explaining-one-row.md) — "why is THIS one at risk": exact per-row TreeSHAP for one account, recomputed from the Model, contributions that sum to the score (`explain`, #948 step 6).
- [tree-shap.md](tree-shap.md) — exact path-dependent TreeSHAP over the trained trees: per-entity drivers + an allreduced global mean-|φ| ranking (`tree_shap`, #393).
- [native-oracle.md](native-oracle.md) — the native correctness oracle: committed goldens + analytic micro-tests + cross-cluster equivalence, no reference runtime (`load_golden_oracle`, RSK-8 / #395).
- [integration.md](integration.md) — integration branch merge order, primary solution ownership, diff audit and native proof (#1082).
- [workspace-methodologies.md](workspace-methodologies.md) — a workspace's own methodology from Python code (#1261, lake#655): the replicated `W-` entries beside the read-only catalogue, `register/update/delete/get_methodology`, `source`/`editable` on every listed entry, the pipeline step Verification refuses until validated and `run` hands to Lake's runner (`runner_required`), and `record_methodology_run` registering the Model. The engine never runs the code.
- [standalone-models.md](standalone-models.md) — a model made on its own, not as a project pipeline's step: `train_model`, `retrain_model`, `register_model` (a brought model file), `move_model_to_project`, and the internal project that holds it and is never listed (lake#654).
- [methodologies.md](methodologies.md) — the 21-entry methodology registry, compile resolution, run dispatch, and extension contract (#1061).
- [Data-first featurization](../data/featurization.md) — all 21 methodologies enforce run preconditions and report native feature/exclusion provenance, including grouped posteriors, stacking, labelling rules, private inputs, and event windows; numeric measures are never identifiers by value alone (#1062, #1216).
- [entity_resolution.md](entity_resolution.md) — M-2 keyed single-source projection or distributed fuzzy owned-pair blocking and scoring, allreduced EM, merged conflict-safe components, result tables, and metrics (#1041, #1072, #1204).
- [next_best_action.md](next_best_action.md) — M-8 propensity composition, implicit-feedback ALS, hard policy constraints, recommendation tables, and offline IPS/DR evaluation (#1047).
- [rank.md](rank.md) — M-9 native LambdaMART: whole-group validation, distributed histogram training, ranked-queue tables, NDCG@k, and yield at budget (#1048).
- [vision.md](vision.md) — M-13 native PNG/JPEG decoding, frozen ViT embeddings, source-disjoint distributed head training, prediction tables, and evaluation reports (#1052).
- [document_extraction.md](document_extraction.md) — M-11 native PDF text recovery and TrOCR line recognition, declared-field extraction, evidence retention, review routing, and field-level metrics (#1050).
- [language.md](language.md) — M-10 native WordPiece/BERT embeddings, retrieval, grounded answers, classifier tuning, metrics, and governance rules (#1049).
- [speech.md](speech.md) — M-14 native WAV transcription, Whisper tiny/base checkpoints, consent and privacy rules, transcript tables, and word error rate (#1053).
- [bandit.md](bandit.md) — M-15 native Thompson sampling, LinUCB, and linear Thompson sampling with allreduced per-arm state, constrained propensities, persisted policy tables, and registry execution (#1054).
- [optimization.md](optimization.md) — M-16 native revised-simplex LP and best-bound branch-and-bound MIP: scenario table in, independently verified plan and constraint trace out (#1055).
- [bayesian.md](bayesian.md) — M-17 native split-conformal intervals / classification sets and distributed Gibbs partial pooling, with measured hold-out coverage (#1056).
- [label-efficiency.md](label-efficiency.md) — M-19 active learning plus a native weak-supervision label model, with queryable label-queue and probabilistic-label outputs (#1058).
- [synthetic_private.md](synthetic_private.md) — M-20 native Gaussian-copula synthesis, disclosure-risk evaluation, TSTR utility, and differentially private histogram GBDT with per-run accounting (#1059).
- [streaming.md](streaming.md) — M-21 event-time windows, online logistic SGD, prequential metrics, drift indicators, distributed gradients, topics, output tables, and correctness rules (#1060).

## Methodology portfolio

This table is generated from `zetta::ml::methodologies()`. Required input roles
carry `*`. Regenerate it with
`ZETTA_WRITE_METHODOLOGIES=1 ./build/tests/zetta-tests --gtest_filter=MethodologyRegistry.DocumentationTableMatchesTheRegistry`.

<!-- methodology-table:start -->
| ID | Methodology | Family | Model kinds | Solutions | Inputs | Outputs | Metrics | Status | Issue |
|---|---|---|---|---|---|---|---|---|---|
| M-1 | Tabular supervised learning | The tabular decision core | gbdt, gradient_boosting, classifier, classification, regression, score, tabular | S-1, S-2, S-3, S-4, S-10, S-12, B-2, B-5, B-6, B-7 | entity*, label*, time | model, scores, drivers, ranked entities | held_out_auc, held_out_log_loss, held_out_accuracy, held_out_rmse, held_out_mae | runnable | — |
| M-2 | Entity resolution | Identity & structure | entity_resolution, entity-resolution | — | entity*, group*, label, time | resolved entities, match pairs, review band | pair precision, pair recall, B-cubed precision, B-cubed recall, blocking recall, merge rate, review-band size | runnable | — |
| M-3 | Segmentation / clustering | Identity & structure | segmentation, clustering, cluster | S-9 | entity*, feature*, label, time | model, segment assignments, segment profiles, run report | chosen k, silhouette, Davies-Bouldin, stability (ARI), inertia per k, segment size/share | runnable | [#1042](https://github.com/oramasearch/zetta/issues/1042) |
| M-4 | Time-series forecasting | Time & vigilance | forecast, forecasting, time_series | S-5, S-6 | time*, label*, group | forecast table with prediction intervals, forecast model | MAE, MASE, sMAPE, seasonal-naive MAE, forecast value added, interval coverage | runnable | [#1043](https://github.com/oramasearch/zetta/issues/1043) |
| M-5 | Survival / time-to-event | The tabular decision core | survival, time_to_event | — | entity*, time*, label*, feature_time*, feature* | survival table, interval hazards, expected time to event, median survival, interval calibration | censoring-aware concordance, IPCW integrated Brier score | runnable | — |
| M-6 | Causal inference & uplift | Action & decision | causal, uplift | — | entity*, treatment*, label*, time, feature* | uplift model, treatment effects, targeting policy, Qini curve, run report | held-out AUUC, Qini coefficient, uplift at targeting fraction | runnable | [#1045](https://github.com/oramasearch/zetta/issues/1045) |
| M-7 | Anomaly detection | Time & vigilance | anomaly, anomaly_detection, outlier | S-7, S-8, B-8 | feature*, entity, label, time | anomaly table, budget report, model, lineage | threshold, contamination, alert_budget, alert_rate, precision_at_k | runnable | [#1046](https://github.com/oramasearch/zetta/issues/1046) |
| M-8 | Recommendation / next-best-action | Action & decision | next_best_action, recommendation, recommender | S-11 | entity*, treatment*, label*, time*, feature*, group | recommendation table, policy report, next-best-action model | hit rate at k, popularity hit rate at k, inverse propensity value, doubly robust value, effective sample size, maximum clipped weight | runnable | [#1047](https://github.com/oramasearch/zetta/issues/1047) |
| M-9 | Learning-to-rank | Action & decision | ranking, learning_to_rank, rank | — | entity*, group*, label*, time, feature* | ranked-queue table, priority scores, model | NDCG@k, yield at budget | runnable | [#1048](https://github.com/oramasearch/zetta/issues/1048) |
| M-10 | NLP / embeddings / RAG / fine-tuning | Language, documents & perception | language, nlp, embedding, embeddings, rag, fine_tuning, retrieval | B-3 | entity*, text*, label*, group | embeddings table, retrieval table, grounded answers table, classifier head | recall at k, MRR, groundedness | runnable | [#1049](https://github.com/oramasearch/zetta/issues/1049) |
| M-11 | Document intelligence | Language, documents & perception | document_extraction, document_intelligence, ocr, document | document_automation | document*, label | extracted fields, review queue, evidence links | field precision, field recall, field F1, review share | runnable | [#1050](https://github.com/oramasearch/zetta/issues/1050) |
| M-12 | Graph ML | Identity & structure | graph, graph_ml | — | entity*, group*, feature*, time*, label* | node feature table, graph summary, node scores, graph embeddings | held-out node accuracy, component count, degree distribution, PageRank top nodes | runnable | [#1051](https://github.com/oramasearch/zetta/issues/1051) |
| M-13 | Computer vision | Language, documents & perception | computer_vision, vision | — | image*, label*, group* | vision model, prediction table, confusion matrix | accuracy, macro-F1 | runnable | — |
| M-14 | Speech & audio analytics | Language, documents & perception | speech, audio | — | audio*, group*, label | transcript table, speech model | word error rate | runnable | [#1053](https://github.com/oramasearch/zetta/issues/1053) |
| M-15 | Bandits & sequential decisioning | Action & decision | bandit, bandits, sequential_decisioning | — | entity*, treatment*, label*, feature | allocation policy, decision propensities | cumulative reward, regret, exploration share | runnable | — |
| M-16 | Optimization / operations research | Action & decision | optimization, operations_research | — | variable*, objective*, constraint*, scenario*, group | plan, constraint trace, solver status | objective value, current plan objective, objective delta, constraint violations, optimality gap | runnable | [#1055](https://github.com/oramasearch/zetta/issues/1055) |
| M-17 | Bayesian / probabilistic modeling & uncertainty quantification | The tabular decision core | bayesian, probabilistic, uncertainty | — | entity*, label*, group*, time, prediction | posterior estimates, credible intervals, prediction intervals, prediction sets | empirical coverage, nominal coverage, mean interval width, mean prediction-set size | runnable | [#1056](https://github.com/oramasearch/zetta/issues/1056) |
| M-18 | Ensembling & stacking | The tabular decision core | ensemble, ensembling, stacking | — | entity*, feature*, label*, time | ensemble model, blended scores, meta-model weights | held_out_auc, held_out_log_loss, held_out_auc_lift_over_best_single | runnable | — |
| M-19 | Label efficiency | The data supply chain | label_efficiency, active_learning, weak_supervision | label-efficient-training, support-ticket-triage | entity*, feature*, label* | label queue table, weak-label table | accuracy at labelling budget, labels saved, label-model gold accuracy | runnable | — |
| M-20 | Privacy-preserving learning & synthetic data | The data supply chain | synthetic_private, privacy_preserving, synthetic_data | — | feature*, label*, group | synthetic table, DP model, guarantee report | marginal distance, correlation distance, DCR, membership AUC, TSTR AUC, epsilon, delta | runnable | [#1059](https://github.com/oramasearch/zetta/issues/1059) |
| M-21 | Online / streaming learning | Time & vigilance | online_learning, streaming | — | entity*, time*, feature*, label, group | online model, streaming scores, score table, run report | prequential log loss, prequential accuracy, drift loss delta, late events excluded | runnable | [#1060](https://github.com/oramasearch/zetta/issues/1060) |
<!-- methodology-table:end -->
