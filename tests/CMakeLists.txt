# Unit tests — GoogleTest, fetched like the rest of the deps.

include(FetchContent)

set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)  # we only need gtest
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(zetta-tests
  api/test_registry.cpp
  api/test_consent.cpp
  api/test_auth_gate.cpp
  api/test_mcp_server.cpp
  api/test_rest_server.cpp
  api/test_openapi.cpp
  api/test_llms.cpp
  api/test_specs.cpp
  api/test_search_journey.cpp
  api/test_create_index_journey.cpp
  api/test_action_journeys.cpp
  api/test_serving.cpp
  api/test_scoped_grants_api.cpp
  api/test_row_security_api.cpp
  util/test_uuid.cpp
  util/test_sha256.cpp
  util/test_fuzzy.cpp
  pipeline/test_validation.cpp
  pipeline/test_gate.cpp
  pipeline/test_graph_checks.cpp
  usecases/churn/test_entity_resolution_check.cpp
  usecases/churn/test_entity_resolution.cpp
  util/test_duration.cpp
  util/test_timestamp.cpp
  data/test_jsonl_store.cpp
  data/test_partitioner.cpp
  data/test_rendezvous_placement.cpp
  data/test_database.cpp
  data/test_replication_strategy.cpp
  pipeline/test_project.cpp
  pipeline/test_project_input.cpp
  pipeline/test_project_graph.cpp
  pipeline/test_deployment.cpp
  pipeline/test_deployment_controller.cpp
  cloud/test_provisioner.cpp
  cloud/test_provisioner_detect.cpp
  cloud/test_node_bootstrap_env.cpp
  cloud/test_add_nodes.cpp
  cloud/test_local_provisioner.cpp
  cloud/test_azure_provisioner.cpp
  cloud/test_aws_provisioner.cpp
  cloud/test_gcp_provisioner.cpp
  cloud/test_oci_provisioner.cpp
  cloud/test_k8s_provisioner.cpp
  cloud/test_bare_metal_provisioner.cpp
  cloud/test_burst_identity.cpp
  compute/test_scaling_policy.cpp
  cloud/test_burst_approval_gate.cpp
  compute/test_job_autoscaler.cpp
  pipeline/test_solution_catalog.cpp
  cloud/test_cloud_provider.cpp
  cloud/test_cloud_inspector.cpp
  pipeline/test_run.cpp
  ml/test_model.cpp
  index/test_index.cpp
  index/test_index_step_executor.cpp
  index/test_oramacore_fulltext_index.cpp
  index/test_distributed_fulltext.cpp
  index/test_node_search.cpp
  compute/test_compute_job.cpp
  compute/test_checkpoint.cpp
  data/test_dataset_partition.cpp
  data/test_partition_writer.cpp
  data/test_partition_rebalance.cpp
  governance/test_access_control.cpp
  governance/test_users_roles_tokens.cpp
  governance/test_roles.cpp
  governance/test_scoped_grants.cpp
  governance/test_row_security.cpp
  governance/test_consent.cpp
  pipeline/test_executor.cpp
  pipeline/test_run_progress.cpp
  usecases/churn/test_churn_step_executor.cpp
  usecases/churn/test_churn_solution.cpp
  usecases/churn/test_churn_coordinated_launch.cpp
  usecases/churn/test_uc1_end_to_end.cpp
  testkit/test_testkit.cpp
  testkit/test_example_scripts.cpp
  governance/test_audit.cpp
  observability/test_activity.cpp
  pipeline/test_intent_compiler.cpp
  pipeline/test_graph_plan.cpp
  test_node.cpp
  cluster/test_node_metrics.cpp
  cluster/test_node_telemetry.cpp
  compute/test_pipeline_instrumentation.cpp
  observability/test_trace_views.cpp
  observability/test_crash_reports.cpp
  data/test_query.cpp
  repl/test_repl_console.cpp
  repl/test_governed_console.cpp
  pipeline/test_execution_graph.cpp
  ddf/test_data_frame.cpp
  ddf/test_ddf_ops.cpp
  ddf/test_ddf_join.cpp
  ddf/test_data_frame_ops.cpp
  ddf/test_distributed_sort.cpp
  ddf/test_group_by.cpp
  ddf/test_ddf_as_of.cpp
  ddf/test_ddf_group_by.cpp
  ddf/test_ddf_local_partitions.cpp
  ddf/test_ddf_featurize.cpp
  ddf/test_ddf_featurize_partitions.cpp
  ml/test_gbdt.cpp
  ml/test_gbdt_train_partitions.cpp
  ml/test_gbdt_featurize_train_partitions.cpp
  ml/test_calibration.cpp
  ml/test_tree_shap.cpp
  ml/test_native_oracle.cpp
  ml/test_gbdt_score.cpp
  ml/test_playbook.cpp
  compute/test_compute_device.cpp
  compute/test_cpu_device.cpp
  compute/test_cuda_device.cpp
  compute/test_device_registry.cpp
  data/test_large_object.cpp
  data/test_import.cpp
  data/test_add_dataset.cpp
  connect/test_connector.cpp
  usecases/churn/test_churn_dataset.cpp
  pipeline/test_n8n_import.cpp
  pipeline/test_n8n_compile.cpp
  data/test_parquet_metadata.cpp
  data/test_parquet_snappy.cpp
  data/test_parquet_column_decoder.cpp
  data/test_parquet_reader.cpp
  pipeline/test_python_frontend.cpp
  pipeline/test_python_engine_bindings.cpp
  pipeline/test_datasets_bindings.cpp
  test_runtime.cpp
  compute/test_work_stealing_scheduler.cpp
  compute/test_distributed_scheduler.cpp
  compute/test_coordinated_launch.cpp
  compute/test_job_coordinator.cpp
  compute/test_node_job_coordinator.cpp
  cluster/test_cluster.cpp
  cluster/test_listen_port.cpp
  cluster/test_cluster_routing.cpp
  cluster/test_cluster_security.cpp
  cluster/test_signal.cpp
  cluster/test_collectives.cpp
  cluster/test_replication.cpp
  cluster/test_data_replication.cpp
  data/test_large_object_replication.cpp
  test_integrity.cpp
  test_schemas.cpp
)

target_compile_features(zetta-tests PRIVATE cxx_std_26)
target_include_directories(zetta-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Path to test fixture files (e.g. the n8n churn workflow) so tests can load
# them at runtime without depending on the working directory; and the repo's
# schemas/ dir, the generated home of the schema-as-code JSON Schemas (test_schemas).
target_compile_definitions(zetta-tests PRIVATE
  ZETTA_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  ZETTA_SCHEMAS_DIR="${CMAKE_SOURCE_DIR}/schemas")
target_link_libraries(zetta-tests
  PRIVATE zetta-api PRIVATE zetta-core GTest::gtest_main
  # The boundary validator, so schema-as-code tests can assert every
  # registered input/output schema actually loads (test_openapi.cpp).
  PRIVATE nlohmann_json_schema_validator)

include(GoogleTest)
gtest_discover_tests(zetta-tests)
