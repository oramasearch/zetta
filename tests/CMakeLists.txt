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
  test_duration.cpp
  test_jsonl_store.cpp
  test_database.cpp
  test_node.cpp
  test_query.cpp
  test_large_object.cpp
  test_import.cpp
  test_runtime.cpp
  test_cluster.cpp
  test_replication.cpp
)

target_compile_features(zetta-tests PRIVATE cxx_std_26)
target_include_directories(zetta-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zetta-tests PRIVATE zetta-core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(zetta-tests)
