#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

// The concrete correctness checks for #309 — the first real semantic checks that
// plug into the CorrectnessGate's `Check` interface (gate.hpp) without touching the
// gate itself. They read a small, documented set of **annotations** from each
// step's free-form `attributes` bag (see schemas/graph_step_annotations.schema.json
// and docs/pipeline/gate-checks.md) and turn a mistyped or leaky graph into a clear-reason
// gate failure (RULE-1 — no deploy without a passing gate).
//
// Both validate the annotations they read against the JSON Schema at the
// boundary
// (`graph_step_annotations_schema()` below), so malformed annotations
// fail fast with a precise reason rather than being silently misread. A step
// that carries none of the relevant annotations is simply skipped (it passes),
// so these checks are opt-in per step and never penalise a graph that doesn't
// use them.
namespace zetta::pipeline {

// M-4's executable shape and mandatory correctness contract, including a
// temporal split, strictly past features and a seasonal-naive baseline.
class ForecastGraphCheck final : public Check {
  public:
    std::string name() const override;
    CheckResult evaluate(const ir::ExecutionGraph &graph) const override;
};

// M-7 (#1046): labels are evaluation-only, the budget is explicit, scales
// are robust, and KPI baselines use only prior observations.
class AnomalyGraphCheck final : public Check {
public:
  std::string name() const override {
    return "anomaly_correctness";
  }
  CheckResult evaluate(const ir::ExecutionGraph &graph) const override;
};

// Schema/type check (#309): every field a step `requires` from its inputs must be
// provided — with a matching type — by the `schema` (source profile) of one of
// the step's transitive data-flow ancestors. A required field that no upstream
// source provides, or that is provided under a different type, fails the gate.
// Type tokens are opaque strings compared case-insensitively.
// Methodology check (#984): a model step must name a methodology the engine can actually
// execute. Until `model_kind` rode the model step it was a string in a bag nothing read, so a
// graph could name `forecast` and run the churn path — the failure was SILENT, and a silent
// wrong pipeline is the product lying fluently. A loud refusal is the honest outcome.
//
// The set is deliberately narrow: what `src/core/ml/` implements TODAY, not what the catalogue
// specifies. It widens as each M-n lands (#984). A model step with no `model_kind` is skipped
// rather than failed — the annotation is opt-in, exactly like the other checks here — but a
// model step that names an UNKNOWN methodology fails, because the alternative is the agent
// proposing work the engine will silently not do.
// Temporal-validation check (#984, M-4). A methodology whose data is time-ordered must be
// validated over time and must NOT be calibrated.
//
// Both halves matter and the second is the counter-intuitive one. Calibration maps scores to
// probabilities by fitting on held-out rows — correct and necessary for churn. On a forecast it
// is a category error: there is no probability to calibrate, and fitting a calibrator over a
// time series held out by row is the same leak `temporal_split` exists to prevent, wearing a
// respectable name. So a forecasting graph that carries `calibrated: true` is not "extra rigour",
// it is the churn recipe applied to a problem it does not fit — precisely the confusion the
// corpus's neighbouring-solution negative control is trying to teach against.
//
// Opt-in like every check here: a graph whose model step names no methodology is skipped.
class TemporalValidationCheck final : public Check {
public:
    std::string name() const override { return "temporal_validation"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;

    // Methodologies whose data is time-ordered, so temporal validation is required and
    // calibration is refused.
    static bool is_time_ordered(std::string_view model_kind);
};

class MethodologyCheck final : public Check {
public:
    std::string name() const override { return "methodology"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;

    // The methodologies the engine can execute today. Widened by #984, never by a caller —
    // a per-call override is how "the engine supports it" becomes unfalsifiable.
    static const std::vector<std::string>& implemented();
};

// M-2 refuses graphs that omit the information needed to enforce its runtime
// safety rules. Blocking keys and both decision thresholds must be explicit,
// and clustering must declare the verified-identifier conflict policy.
class EntityResolutionGraphCheck final : public Check {
public:
    std::string name() const override { return "entity_resolution_profile"; }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// Survival-censoring check (#1044): a survival graph must declare that censored
// observations remain non-events, person-period expansion stops after an event,
// and feature values are bounded by their subject cutoff.
class SurvivalCensoringCheck final : public Check {
public:
    std::string name() const override { return "survival_censoring"; }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-8 policy correctness (#1047): only the validated next-best-action graph is
// accepted. It must retain chronological interaction holdout, explicit logging
// propensity handling, eligibility/contact-cap constraints and budgeted
// best-action selection.
class NextBestActionGraphCheck final : public Check {
public:
    std::string name() const override { return "next_best_action_correctness"; }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-11 document extraction must never accept a field without a confidence,
// source evidence, and an explicit low-confidence review route.
class DocumentReviewCheck final : public Check {
public:
    std::string name() const override {
        return "document_review";
    }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-10 language outputs may never answer without retrieved evidence, and every
// answer sentence must carry a citation. The executable graph records both
// invariants explicitly so an edited plan cannot silently weaken them.
class LanguageGroundingCheck final : public Check {
public:
    std::string name() const override { return "language_grounding"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-10 may not train on text outside the caller's read scope. This graph-level
// check requires the model and its source to opt into the runtime authorization
// check; the methodology runner then performs that check before reading rows.
class LanguageTrainingAccessCheck final : public Check {
public:
    std::string name() const override { return "language_training_access"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-16 requires scenario inputs, independent verification and solver status.
class OptimizationContractCheck final : public Check {
public:
    std::string name() const override {
        return "optimization_contract";
    }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class UncertaintyValidationCheck final : public Check {
public:
    std::string name() const override { return "uncertainty_validation"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-18 correctness profile (#1057): the meta-model may only learn from
// out-of-fold predictions, bases must be refit over all non-holdout training
// rows, and time-dependent folds must validate strictly forward in time.
class EnsembleOutOfFoldCheck final : public Check {
public:
    std::string name() const override {
        return "ensemble_out_of_fold";
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-20 privacy-budget check (#1059): a synthetic_private model step must state
// epsilon/delta and enforced maxima, must stay within those maxima, must state
// public clipping bounds for the DP histogram sensitivity, and must require the
// privacy evaluation that controls whether a synthetic table may be published.
class PrivacyBudgetCheck final : public Check {
public:
    std::string name() const override { return "privacy_budget"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// Streaming correctness check (#1060): M-21 must order work behind an
// event-time watermark, reject negative lateness, withhold labels until their
// own event time, and measure predictions before training on those labels.
// These annotations are required on every streaming model step; runtime code
// independently applies the same policy to each event.
class StreamingCorrectnessCheck final : public Check {
public:
    std::string name() const override { return "streaming_correctness"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class SchemaTypeCheck final : public Check {
public:
    std::string name() const override { return "schema_type"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-3 must keep entity/outcome roles out of features and execute its own
// globally fitted preprocessing and assignment/profile output graph.
class SegmentationCheck final : public Check {
  public:
    std::string name() const override;
    CheckResult evaluate(const ir::ExecutionGraph &graph) const override;
};

// Data-leakage / point-in-time check (#309): for an ML step that declares
// `features` (with an `as_of` cutoff), no feature may draw on data dated after
// the cutoff (`data_as_of` > `as_of` — a point-in-time violation) and no feature
// may be the target or be derived from it (`derived_from` / `leaks_target` /
// matching the `label` — target leakage). Either makes the model train on
// information it would not have at prediction time, so the gate fails.
class DataLeakageCheck final : public Check {
public:
    std::string name() const override { return "data_leakage"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// Point-in-time feature lineage (#1103): every feature-bearing model must trace
// through data-flow edges to snapshot sources, each snapshot cutoff must be at
// or before prediction time T, and the label window must start after the
// feature window plus the declared blackout interval.
class SnapshotLineageCheck final : public Check {
public:
    std::string name() const override { return "snapshot_lineage"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-1's domain adaptations (#1110). These inspect the complete composed
// Pipeline because the manifest, point-in-time sources, and level-specific
// methodology stages jointly establish the retention contract.
class S1PointInTimeCheck final : public Check {
public:
    std::string name() const override { return "s1_point_in_time"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1LabelWindowCheck final : public Check {
public:
    std::string name() const override { return "s1_label_window"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1TemporalValidationCheck final : public Check {
public:
    std::string name() const override { return "s1_temporal_validation"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1GroupSplitCheck final : public Check {
public:
    std::string name() const override { return "s1_group_split"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1CensoringCheck final : public Check {
public:
    std::string name() const override { return "s1_censoring"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1CalibrationCheck final : public Check {
public:
    std::string name() const override { return "s1_calibration"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1EntityResolutionCheck final : public Check {
public:
    std::string name() const override { return "s1_entity_resolution"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1LeakageReviewCheck final : public Check {
public:
    std::string name() const override { return "s1_leakage_review"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S1LevelCompositionCheck final : public Check {
public:
    std::string name() const override { return "s1_level_composition"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-2's domain adaptations (#1111). Conversion outcomes must be observed after
// the score cutoff, post-qualification CRM fields may not become predictors,
// and routed tasks must name the capacity evidence used to select an owner.
class S2PointInTimeConversionCheck final : public Check {
public:
    std::string name() const override { return "s2_point_in_time_conversion"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S2PostQualificationLeakageCheck final : public Check {
public:
    std::string name() const override { return "s2_post_qualification_leakage"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S2CapacityRoutingCheck final : public Check {
public:
    std::string name() const override { return "s2_capacity_routing"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-3's customer-value correctness adaptations (#1112). The module refuses
// immature value labels, an unstated prediction horizon, and entity accounting
// that can count one customer more than once.
class S3ShortTenureCensoringCheck final : public Check {
public:
    std::string name() const override {
        return "s3_short_tenure_censoring";
    }
    bool evaluates_composed_graph() const noexcept override {
        return true;
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S3ValueHorizonCheck final : public Check {
public:
    std::string name() const override {
        return "s3_value_horizon";
    }
    bool evaluates_composed_graph() const noexcept override {
        return true;
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S3EntityAccountingCheck final : public Check {
public:
    std::string name() const override {
        return "s3_entity_accounting";
    }
    bool evaluates_composed_graph() const noexcept override {
        return true;
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-4 collections correctness adaptations (#1113). Regulatory eligibility
// and frequency limits, vulnerable-customer protection, and point-in-time
// promise-to-pay labels are deployment refusals on the composed Pipeline.
class S4RegulatoryContactCheck final : public Check {
public:
    std::string name() const override { return "s4_regulatory_contact"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S4VulnerableCustomerCheck final : public Check {
public:
    std::string name() const override { return "s4_vulnerable_customer"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S4PromiseToPayPointInTimeCheck final : public Check {
public:
    std::string name() const override { return "s4_promise_to_pay_point_in_time"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// B-5 supplier-risk adaptations (#1179). External observations must be frozen
// at the decision cutoff, supplier records after a disruption cannot become
// predictors, and every emitted risk flag must carry source and model evidence.
class B5PointInTimeExternalDataCheck final : public Check {
public:
    std::string name() const override {
        return "b5_point_in_time_external_data";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B5PostEventSupplierLeakageCheck final : public Check {
public:
    std::string name() const override {
        return "b5_post_event_supplier_leakage";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B5FlagEvidenceCheck final : public Check {
public:
    std::string name() const override { return "b5_flag_evidence"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-12 claim-routing adaptations (#1121). These checks keep claim denial out
// of automatic paths, require a documented cohort fairness review, enforce the
// M-11 field-confidence boundary, and verify every selected capability level.
class S12NoAutomatedDenialCheck final : public Check {
public:
    std::string name() const override { return "s12_no_automated_denial"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S12FairnessReviewCheck final : public Check {
public:
    std::string name() const override { return "s12_fairness_review"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S12DocumentConfidenceCheck final : public Check {
public:
    std::string name() const override { return "s12_document_confidence"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S12LevelCompositionCheck final : public Check {
public:
    std::string name() const override { return "s12_level_composition"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// B-3 service automation adaptations (#1177). The composed Pipeline must
// abstain below a governed confidence floor, use a customer-owned taxonomy,
// keep closure behind approval, and monitor previously unseen ticket types.
class B3AbstentionCheck final : public Check {
public:
    std::string name() const override { return "b3_abstention_threshold"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B3CustomerTaxonomyCheck final : public Check {
public:
    std::string name() const override { return "b3_customer_taxonomy"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B3ClosureApprovalCheck final : public Check {
public:
    std::string name() const override { return "b3_closure_approval"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B3NewTicketTypeDriftCheck final : public Check {
public:
    std::string name() const override { return "b3_new_ticket_type_drift"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B3LevelCompositionCheck final : public Check {
public:
    std::string name() const override { return "b3_level_composition"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// B-4 document-workflow adaptations (#1178). Every declared field has its own
// threshold, uncertain fields enter an owned human-review queue, and business
// workflow actions remain unavailable until every required field validates.
class B4PerFieldConfidenceCheck final : public Check {
public:
    std::string name() const override { return "b4_per_field_confidence"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B4ReviewQueueCheck final : public Check {
public:
    std::string name() const override { return "b4_review_queue"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B4ValidatedWorkflowCheck final : public Check {
public:
    std::string name() const override { return "b4_validated_workflow"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// B-6 healthcare-operations adaptations (#1180). The module requires an
// owned protected-group review, permits assistive work only, records an
// approved privacy posture, and limits model features to the reviewed
// minimum-necessary set.
class B6EquityReviewCheck final : public Check {
public:
    std::string name() const override { return "b6_equity_review"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B6AssistiveOnlyCheck final : public Check {
public:
    std::string name() const override { return "b6_assistive_only"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B6PrivacyPostureCheck final : public Check {
public:
    std::string name() const override { return "b6_privacy_posture"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B6PhiMinimizationCheck final : public Check {
public:
    std::string name() const override { return "b6_phi_minimization"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};
// S-11 validates logged probabilities, contact constraints, and interpretation.
class S11LoggedPropensityCheck final : public Check {
public:
    std::string name() const override { return "s11_logged_propensity"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S11ContactConsentCheck final : public Check {
public:
    std::string name() const override { return "s11_contact_consent"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S11PropensityInterpretationCheck final : public Check {
public:
    std::string name() const override {
        return "s11_propensity_interpretation";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// B-2 employee-attrition adaptations (#1176). Every surfaced row is a
// protected cohort, review and approval evidence is explicit, and delivery is
// restricted to the named people leader.
class B2MinimumGroupSizeCheck final : public Check {
public:
    std::string name() const override { return "b2_minimum_group_size"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B2AggregateOnlyCheck final : public Check {
public:
    std::string name() const override { return "b2_aggregate_only"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B2FairnessReviewCheck final : public Check {
public:
    std::string name() const override { return "b2_fairness_review"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B2ApprovalCheck final : public Check {
public:
    std::string name() const override { return "b2_approval"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B2PeopleLeaderOutputCheck final : public Check {
public:
    std::string name() const override { return "b2_people_leader_output"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-10 validates label maturity, selection bias, and request-time parity.
class S10LabelDelayCheck final : public Check {
public:
    std::string name() const override { return "s10_label_delay"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S10DeclinedSelectionBiasCheck final : public Check {
public:
    std::string name() const override {
        return "s10_declined_selection_bias";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S10RequestTimeFeatureParityCheck final : public Check {
public:
    std::string name() const override {
        return "s10_request_time_feature_parity";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S10LevelCompositionCheck final : public Check {
public:
    std::string name() const override { return "s10_level_composition"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-5's domain adaptations (#1114). The methodology check proves M-4's own
// shape; these checks bind its temporal contract to the complete demand
// solution and require a coherent leaf-level hierarchy declaration.
class S5RollingOriginCheck final : public Check {
public:
    std::string name() const override { return "s5_rolling_origin"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S5FutureCovariateCheck final : public Check {
public:
    std::string name() const override { return "s5_future_covariates"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S5HierarchyConsistencyCheck final : public Check {
public:
    std::string name() const override { return "s5_hierarchy_consistency"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S5LevelCompositionCheck final : public Check {
public:
    std::string name() const override { return "s5_level_composition"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-6's domain adaptations (#1115) validate the complete replenishment
// Pipeline: service and spend constraints, realistic lead-time coverage, and
// approval protection above the automatic-release quantity bound.
class S6ServiceBudgetCheck final : public Check {
public:
    std::string name() const override {
        return "s6_service_budget_constraints";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S6LeadTimeCheck final : public Check {
public:
    std::string name() const override { return "s6_lead_time_realism"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S6OrderApprovalCheck final : public Check {
public:
    std::string name() const override { return "s6_order_approval"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S6LevelCompositionCheck final : public Check {
public:
    std::string name() const override { return "s6_level_composition"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// B-1 pricing adaptations (#1175). Recommendations require identifiable
// variation, bounded policy authority, decision-time price features, and the
// complete M-4/M-6/M-16 composition at every capability level.
class B1ElasticityIdentificationCheck final : public Check {
public:
    std::string name() const override {
        return "b1_elasticity_identification";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B1MarginAuthorityPolicyCheck final : public Check {
public:
    std::string name() const override {
        return "b1_margin_authority_policy";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B1PriceLeakageCheck final : public Check {
public:
    std::string name() const override { return "b1_price_leakage"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class B1LevelCompositionCheck final : public Check {
public:
    std::string name() const override { return "b1_level_composition"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-7's domain adaptations (#1116). Driver language must remain
// associational, anomaly delivery must obey an explicit alert budget, and KPI
// scoring must use causal seasonal baselines rather than future observations.
class S7CausalHumilityCheck final : public Check {
public:
    std::string name() const override { return "s7_causal_humility"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S7AlertBudgetCheck final : public Check {
public:
    std::string name() const override { return "s7_alert_budget"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S7SeasonalityBaselineCheck final : public Check {
public:
    std::string name() const override { return "s7_seasonality_baseline"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-8's predictive-maintenance adaptations (#1117). Censored assets remain in
// the survival cohort, sensor drift has a pre-decision baseline, and failure or
// repair facts recorded after failure cannot become predictors.
class S8CensoringCheck final : public Check {
public:
    std::string name() const override { return "s8_censoring"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S8SensorDriftCheck final : public Check {
public:
    std::string name() const override { return "s8_sensor_drift"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S8PostFailureMaintenanceLeakageCheck final : public Check {
public:
    std::string name() const override {
        return "s8_post_failure_maintenance_leakage";
    }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// S-9's domain adaptations (#1118). Outbound audiences require current consent
// and suppression evidence, every journey is bounded by persisted contact caps,
// and levels with randomized history must select on positive uplift rather than
// mistaking response propensity for incremental effect.
class S9ConsentSuppressionCheck final : public Check {
public:
    std::string name() const override { return "s9_consent_suppression"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S9ContactCapsCheck final : public Check {
public:
    std::string name() const override { return "s9_contact_caps"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class S9UpliftOverPropensityCheck final : public Check {
public:
    std::string name() const override { return "s9_uplift_over_propensity"; }
    bool evaluates_composed_graph() const noexcept override { return true; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-6 causal correctness: treatment and outcome roles must be explicit and
// distinct, overlap bounds may not weaken the supported [0.05,0.95] policy,
// and outcome-like or post-treatment features are refused before execution.
class CausalUpliftCheck final : public Check {
public:
    std::string name() const override { return "causal_uplift"; }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// Learning-to-rank correctness (#1048): every rank model names its group,
// assigns whole groups to a holdout side, and declares that every feature is
// available when ranking occurs.
class RankCorrectnessCheck final : public Check {
public:
    std::string name() const override { return "rank_correctness"; }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-12 edge-time check (#1051): graph learning must name a prediction cutoff
// and an explicit filter over the same edge-time column. A graph that omits,
// disables, or disagrees about that filter is refused before execution.
class GraphEdgeTimeCheck final : public Check {
public:
    std::string name() const override { return "graph_edge_time"; }

    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// Vision source-split check (#1052): a vision model must declare that holdout
// assignment is made by a named source column. Frame-level random splitting can
// put near-identical frames from one camera, site, or document in both sets and
// report a score that does not measure generalization to a new source.
class VisionSourceSplitCheck final : public Check {
public:
    std::string name() const override { return "vision_source_split"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class SpeechConsentCheck final : public Check {
public:
    std::string name() const override {
        return "speech_consent";
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class SpeechPiiRedactionCheck final : public Check {
public:
    std::string name() const override {
        return "speech_pii_redaction";
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

class SpeechLocalAudioCheck final : public Check {
public:
    std::string name() const override {
        return "speech_local_audio";
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// Bandit-policy check (#1054, M-15): every bandit model step declares a
// positive exploration floor and feasible default/per-arm caps. This rejects
// a graph that could starve an arm or exceed a guardrail allocation before it
// can run.
class BanditPolicyCheck final : public Check {
public:
    std::string name() const override {
        return "bandit_policy";
    }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-19 must reserve its deterministic gold split for evaluation. A graph that
// permits weak labels to become gold, or does not declare a disjoint split,
// would report label-model quality against its own inferred targets.
class LabelEfficiencyGoldIsolationCheck final : public Check {
public:
    std::string name() const override { return "label_efficiency_gold_isolation"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// The step-annotation JSON Schema (draft-07) — the code-owned source of truth;
// schemas/graph_step_annotations.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& graph_step_annotations_schema();

} // namespace zetta::pipeline
