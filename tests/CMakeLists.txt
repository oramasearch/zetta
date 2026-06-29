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
  test_uuid.cpp
  test_sha256.cpp
  test_validation.cpp
  test_gate.cpp
  test_graph_checks.cpp
  test_entity_resolution_check.cpp
  test_entity_resolution.cpp
  test_duration.cpp
  test_jsonl_store.cpp
  test_partitioner.cpp
  test_rendezvous_placement.cpp
  test_database.cpp
  test_replication_strategy.cpp
  test_project.cpp
  test_project_input.cpp
  test_project_graph.cpp
  test_deployment.cpp
  test_deployment_controller.cpp
  test_provisioner.cpp
  test_add_nodes.cpp
  test_local_provisioner.cpp
  test_azure_provisioner.cpp
  test_aws_provisioner.cpp
  test_gcp_provisioner.cpp
  test_oci_provisioner.cpp
  test_k8s_provisioner.cpp
  test_bare_metal_provisioner.cpp
  test_burst_identity.cpp
  test_scaling_policy.cpp
  test_burst_approval_gate.cpp
  test_job_autoscaler.cpp
  test_solution_catalog.cpp
  test_cloud_provider.cpp
  test_cloud_inspector.cpp
  test_run.cpp
  test_model.cpp
  test_index.cpp
  test_oramacore_fulltext_index.cpp
  test_compute_job.cpp
  test_dataset_partition.cpp
  test_access_control.cpp
  test_executor.cpp
  test_churn_step_executor.cpp
  test_churn_solution.cpp
  test_uc1_end_to_end.cpp
  test_testkit.cpp
  test_example_scripts.cpp
  test_audit.cpp
  test_intent_compiler.cpp
  test_node.cpp
  test_query.cpp
  test_repl_console.cpp
  test_governed_console.cpp
  test_execution_graph.cpp
  test_data_frame.cpp
  test_ddf_ops.cpp
  test_ddf_join.cpp
  test_data_frame_ops.cpp
  test_distributed_sort.cpp
  test_group_by.cpp
  test_ddf_as_of.cpp
  test_ddf_group_by.cpp
  test_ddf_featurize.cpp
  test_gbdt.cpp
  test_calibration.cpp
  test_tree_shap.cpp
  test_native_oracle.cpp
  test_gbdt_score.cpp
  test_playbook.cpp
  test_compute_device.cpp
  test_cpu_device.cpp
  test_cuda_device.cpp
  test_device_registry.cpp
  test_large_object.cpp
  test_import.cpp
  test_churn_dataset.cpp
  test_n8n_import.cpp
  test_n8n_compile.cpp
  test_parquet_metadata.cpp
  test_parquet_snappy.cpp
  test_parquet_column_decoder.cpp
  test_python_frontend.cpp
  test_python_engine_bindings.cpp
  test_runtime.cpp
  test_work_stealing_scheduler.cpp
  test_distributed_scheduler.cpp
  test_job_coordinator.cpp
  test_cluster.cpp
  test_signal.cpp
  test_collectives.cpp
  test_replication.cpp
  test_data_replication.cpp
  test_large_object_replication.cpp
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
target_link_libraries(zetta-tests PRIVATE zetta-core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(zetta-tests)
