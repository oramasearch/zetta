// The boundary-schema registry (#646) — the one table naming every
// schema-as-code boundary schema. Entries are grouped by owning domain;
// each accessor lives beside its entity (the crash_report_schema pattern)
// and this TU only aggregates. tests/test_schemas.cpp regenerates and
// drift-checks schemas/<file> from this table; the spec index
// (src/core/api/specs.cpp) serves it.
#include "zetta/schemas/registry.hpp"

#include <algorithm>

#include "zetta/api/serving.hpp"
#include "zetta/billing/plan.hpp"
#include "zetta/cloud/add_nodes.hpp"
#include "zetta/cloud/aws_provisioner.hpp"
#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/bare_metal_provisioner.hpp"
#include "zetta/cloud/burst_approval_gate.hpp"
#include "zetta/cloud/cloud_inspector.hpp"
#include "zetta/cloud/gcp_provisioner.hpp"
#include "zetta/cloud/k8s_provisioner.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/oci_provisioner.hpp"
#include "zetta/cluster/collectives.hpp"
#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/cluster/signal.hpp"
#include "zetta/compute/checkpoint.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/device_registry.hpp"
#include "zetta/compute/distributed_scheduler.hpp"
#include "zetta/compute/job_autoscaler.hpp"
#include "zetta/compute/scaling_policy.hpp"
#include "zetta/connect/distributed_ingest.hpp"
#include "zetta/data/dataset_manifest.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_scheduler.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/replication_strategy.hpp"
#include "zetta/ddf/as_of.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/distributed_sort.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/ddf/parquet_reader.hpp"
#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/consent.hpp"
#include "zetta/governance/policy.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/governance/user_role.hpp"
#include "zetta/index/distributed_fulltext.hpp"
#include "zetta/index/index.hpp"
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/forecast_methodology.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/golden_oracle.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/next_best_action_methodology.hpp"
#include "zetta/ml/playbook.hpp"
#include "zetta/ml/segmentation_methodology.hpp"
#include "zetta/ml/streaming.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/observability/feedback.hpp"
#include "zetta/parquet/column_decoder.hpp"
#include "zetta/parquet/metadata.hpp"
#include "zetta/pipeline/graph_checks.hpp"
#include "zetta/pipeline/n8n_import.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/solution_catalog.hpp"
#include "zetta/pipeline/validation.hpp"
#include "zetta/query/distributed_select.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/usecases/churn/entity_resolution.hpp"
#include "zetta/workspace/workspace_profile.hpp"

namespace zetta::schemas {

const std::vector<BoundarySchema>& boundary_schemas() {
    static const std::vector<BoundarySchema> registry = {
        // pipeline
        {"run", "run.schema.json", &zetta::pipeline::run_value_schema},
        {"solution_catalog", "solution_catalog.schema.json",
         &zetta::pipeline::solution_catalog_schema},
        {"validation_report", "validation_report.schema.json",
         &zetta::pipeline::validation_report_schema},
        {"graph_step_annotations", "graph_step_annotations.schema.json",
         &zetta::pipeline::graph_step_annotations_schema},
        {"n8n_workflow", "n8n_workflow.schema.json",
         &zetta::pipeline::n8n_workflow_schema},
        // ml
        {"model", "model.schema.json", &zetta::ml::model_value_schema},
        {"segmentation_config", "segmentation_config.schema.json",
         &zetta::ml::segmentation_config_schema},
        {"segmentation_launch", "segmentation_launch.schema.json",
         &zetta::ml::segmentation_launch_schema},
        {"workspace_profile", "workspace_profile.schema.json",
         &zetta::workspace::workspace_profile_value_schema},
        {"plan", "plan.schema.json", &zetta::billing::plan_value_schema},
        // solutions
        {"solution_monitor", "solution_monitor.schema.json",
         &zetta::solutions::monitor_value_schema},
        {"calibration", "calibration.schema.json",
         &zetta::ml::calibration_schema},
        {"gbdt_model", "gbdt_model.schema.json", &zetta::ml::gbdt_model_schema},
        {"gbdt_params", "gbdt_params.schema.json",
         &zetta::ml::gbdt_params_schema},
        {"forecast_step", "forecast_step.schema.json",
         &zetta::ml::forecast_step_schema},
        {"forecast_launch", "forecast_launch.schema.json",
         &zetta::ml::forecast_launch_schema},
        {"streaming_model", "streaming_model.schema.json",
         &zetta::ml::streaming_model_schema},
        {"streaming_params", "streaming_params.schema.json",
         &zetta::ml::streaming_params_schema},
        {"golden_oracle", "golden_oracle.schema.json",
         &zetta::ml::golden_oracle_schema},
        {"playbook_entry", "playbook_entry.schema.json",
         &zetta::ml::playbook_entry_schema},
        {"playbook_policy", "playbook_policy.schema.json",
         &zetta::ml::playbook_policy_schema},
        {"next_best_action_step", "next_best_action_step.schema.json",
         &zetta::ml::next_best_action_step_schema},
        {"next_best_action_launch", "next_best_action_launch.schema.json",
         &zetta::ml::next_best_action_launch_schema},
        // index
        {"index", "index.schema.json", &zetta::index::index_value_schema},
        {"fulltext_query", "fulltext_query.schema.json",
         &zetta::index::fulltext_query_schema},
        {"fulltext_hits", "fulltext_hits.schema.json",
         &zetta::index::fulltext_hits_schema},
        {"partition_scan_request", "partition_scan_request.schema.json",
         &zetta::query::partition_scan_request_schema},
        {"partition_scan_response", "partition_scan_response.schema.json",
         &zetta::query::partition_scan_response_schema},
        // compute
        {"compute_job", "compute_job.schema.json",
         &zetta::compute::compute_job_value_schema},
        {"job_checkpoint", "job_checkpoint.schema.json",
         &zetta::compute::job_checkpoint_schema},
        {"coordinated_launch_request", "coordinated_launch_request.schema.json",
         &zetta::compute::coordinated_launch_request_schema},
        {"autoscaler_config", "autoscaler_config.schema.json",
         &zetta::compute::autoscaler_config_schema},
        {"scaling_policy", "scaling_policy.schema.json",
         &zetta::compute::scaling_policy_schema},
        {"device_registry", "device_registry.schema.json",
         &zetta::compute::device_registry_schema},
        {"steal_request", "steal_request.schema.json",
         &zetta::compute::steal_request_schema},
        {"steal_offer", "steal_offer.schema.json",
         &zetta::compute::steal_offer_schema},
        {"steal_result", "steal_result.schema.json",
         &zetta::compute::steal_result_schema},
        // data
        {"dataset_manifest", "dataset_manifest.schema.json",
         &zetta::data::dataset_manifest_value_schema},
        {"dataset_partition", "dataset_partition.schema.json",
         &zetta::data::dataset_partition_value_schema},
        {"partition_plan", "partition_plan.schema.json",
         &zetta::data::partition_plan_schema},
        {"partitioner", "partitioner.schema.json",
         &zetta::data::partitioner_schema},
        {"replication_strategy", "replication_strategy.schema.json",
         &zetta::data::replication_strategy_schema},
        // ddf
        {"parquet_data_frame", "parquet_data_frame.schema.json",
         &zetta::ddf::parquet_data_frame_schema},
        {"data_frame_schema", "data_frame_schema.schema.json",
         &zetta::ddf::data_frame_schema_schema},
        {"as_of_aggregate", "as_of_aggregate.schema.json",
         &zetta::ddf::as_of_aggregate_schema},
        {"ddf_filter_predicate", "ddf_filter_predicate.schema.json",
         &zetta::ddf::ddf_filter_predicate_schema},
        {"ddf_projection", "ddf_projection.schema.json",
         &zetta::ddf::ddf_projection_schema},
        {"ddf_partition", "ddf_partition.schema.json",
         &zetta::ddf::ddf_partition_schema},
        {"group_by_spec", "group_by_spec.schema.json",
         &zetta::ddf::group_by_spec_schema},
        {"featurize_spec", "featurize_spec.schema.json",
         &zetta::ddf::featurize_spec_schema},
        // parquet
        {"parquet_metadata", "parquet_metadata.schema.json",
         &zetta::parquet::parquet_metadata_schema},
        {"parquet_columns", "parquet_columns.schema.json",
         &zetta::parquet::parquet_columns_schema},
        // cluster
        {"node_metrics", "node_metrics.schema.json",
         &zetta::cluster::node_metrics_wire_schema},
        {"cluster_secret", "cluster_secret.schema.json",
         &zetta::cluster::cluster_secret_schema},
        {"collective_message", "collective_message.schema.json",
         &zetta::cluster::collective_message_schema},
        {"signal_envelope", "signal_envelope.schema.json",
         &zetta::cluster::signal_envelope_schema},
        // governance
        {"audit_event", "audit_event.schema.json",
         &zetta::governance::audit_event_value_schema},
        {"consent_challenge", "consent_challenge.schema.json",
         &zetta::governance::consent_challenge_schema},
        {"access_key", "access_key.schema.json",
         &zetta::governance::access_key_schema},
        {"principal", "principal.schema.json",
         &zetta::governance::principal_schema},
        {"user", "user.schema.json", &zetta::governance::user_schema},
        {"role", "role.schema.json", &zetta::governance::role_schema},
        {"token", "token.schema.json", &zetta::governance::token_schema},
        {"user_role", "user_role.schema.json",
         &zetta::governance::user_role_schema},
        {"policy", "policy.schema.json", &zetta::governance::policy_schema},
        // observability
        {"crash_report", "crash_report.schema.json",
         &zetta::observability::crash_report_schema},
        {"feedback_bundle", "feedback_bundle.schema.json",
         &zetta::observability::feedback_bundle_schema},
        // cloud
        {"cloud_inspect_report", "cloud_inspect_report.schema.json",
         &zetta::cloud::cloud_inspect_report_schema},
        {"add_nodes_request", "add_nodes_request.schema.json",
         &zetta::cloud::add_nodes_request_schema},
        {"node_capability", "node_capability.schema.json",
         &zetta::cloud::node_capability_schema},
        {"burst_cap", "burst_cap.schema.json", &zetta::cloud::burst_cap_schema},
        {"aws_config", "aws_config.schema.json",
         &zetta::cloud::aws_config_schema},
        {"azure_config", "azure_config.schema.json",
         &zetta::cloud::azure_config_schema},
        {"gcp_config", "gcp_config.schema.json",
         &zetta::cloud::gcp_config_schema},
        {"oci_config", "oci_config.schema.json",
         &zetta::cloud::oci_config_schema},
        {"k8s_config", "k8s_config.schema.json",
         &zetta::cloud::k8s_config_schema},
        {"bare_metal_config", "bare_metal_config.schema.json",
         &zetta::cloud::bare_metal_config_schema},
        // connect (the distributed connector ingest, #833)
        {"connector_fetch_launch", "connector_fetch_launch.schema.json",
         &zetta::connect::connector_fetch_launch_schema},
        // usecases/churn
        {"churn_coordinated_run", "churn_coordinated_run.schema.json",
         &zetta::usecases::churn::churn_coordinated_run_schema},
        {"churn_dataset", "churn_dataset.schema.json",
         &zetta::usecases::churn::churn_dataset_schema},
        {"churn_account", "churn_account.schema.json",
         &zetta::usecases::churn::churn_account_schema},
        {"churn_event", "churn_event.schema.json",
         &zetta::usecases::churn::churn_event_schema},
        {"entity_resolution", "entity_resolution.schema.json",
         &zetta::usecases::churn::entity_resolution_schema},
        // api (governed HTTP serving, #648)
        {"predict_request", "predict_request.schema.json",
         &zetta::api::predict_request_schema},
        {"serve_search_request", "serve_search_request.schema.json",
         &zetta::api::serve_search_request_schema},
    };
    return registry;
}

const BoundarySchema* find_boundary_schema(std::string_view name) {
    const auto& registry = boundary_schemas();
    const auto match =
        std::find_if(registry.begin(), registry.end(),
                     [&](const BoundarySchema& entry) { return entry.name == name; });
    return match == registry.end() ? nullptr : &*match;
}

}  // namespace zetta::schemas
