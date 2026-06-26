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
  test_duration.cpp
  test_jsonl_store.cpp
  test_database.cpp
  test_project.cpp
  test_project_input.cpp
  test_project_graph.cpp
  test_deployment.cpp
  test_deployment_controller.cpp
  test_run.cpp
  test_access_control.cpp
  test_node.cpp
  test_query.cpp
  test_execution_graph.cpp
  test_large_object.cpp
  test_import.cpp
  test_n8n_import.cpp
  test_runtime.cpp
  test_cluster.cpp
  test_replication.cpp
  test_data_replication.cpp
  test_large_object_replication.cpp
  test_integrity.cpp
)

target_compile_features(zetta-tests PRIVATE cxx_std_26)
target_include_directories(zetta-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Path to test fixture files (e.g. the n8n churn workflow) so tests can load
# them at runtime without depending on the working directory.
target_compile_definitions(zetta-tests PRIVATE
  ZETTA_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(zetta-tests PRIVATE zetta-core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(zetta-tests)
