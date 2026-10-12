#include "zetta/memory/accounting.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

#include "temp_dir.hpp"
#include "zetta/node.hpp"

namespace zetta::memory {
namespace {

TEST(MemoryAccounting, AggregatesProbesAndReportsUnattributedBytes) {
  MemoryAccounting accounting{[] {
    return AllocatorSample{true, true, 1'000, 200, 1'500, 1'600, 900};
  }};
  accounting.register_probe("audit", [] { return ProbeSample{600, 100, 0}; });
  accounting.register_probe("pubsub", [] { return ProbeSample{300, 10, 0}; });

  const auto snapshot = accounting.snapshot();

  ASSERT_EQ(snapshot.subsystems.size(), 2u);
  EXPECT_EQ(snapshot.attributed_bytes, 900u);
  EXPECT_EQ(snapshot.unattributed_bytes, 100u);
  EXPECT_EQ(snapshot.over_attributed_bytes, 0u);
  EXPECT_GE(snapshot.attributed_bytes * 100 / snapshot.allocator.in_use_bytes,
            90u);
  EXPECT_DOUBLE_EQ(snapshot.subsystems.front().share_of_in_use, 60.0);
}

TEST(MemoryAccounting, CombinesMultipleRegistrationsForOneSubsystem) {
  MemoryAccounting accounting{[] { return AllocatorSample{}; }};
  const auto first = accounting.register_probe(
      "registry_sessions_connections", [] { return ProbeSample{40, 1, 0}; });
  accounting.register_probe("registry_sessions_connections",
                            [] { return ProbeSample{60, 2, 0}; });

  auto snapshot = accounting.snapshot();
  ASSERT_EQ(snapshot.subsystems.size(), 1u);
  EXPECT_EQ(snapshot.subsystems.front().bytes, 100u);
  EXPECT_EQ(snapshot.subsystems.front().items, 3u);

  EXPECT_TRUE(accounting.unregister_probe(first));
  snapshot = accounting.snapshot();
  EXPECT_EQ(snapshot.subsystems.front().bytes, 60u);
}

TEST(MemoryAccounting, FormatsPeriodicLogLine) {
  Snapshot snapshot;
  snapshot.allocator.rss_bytes = 812 * 1024 * 1024;
  snapshot.allocator.in_use_bytes = 790 * 1024 * 1024;
  snapshot.subsystems.push_back(SubsystemSample{"audit", 610 * 1024 * 1024, 1, 0, 0});
  snapshot.unattributed_bytes = 180 * 1024 * 1024;

  const auto line = format_log_line(snapshot);

  EXPECT_NE(line.find("memory: rss 812M in-use 790M audit 610M"), std::string::npos);
  EXPECT_NE(line.find("unattributed 180M"), std::string::npos);
}

TEST(MemoryAccounting, NodeStartupLogsMemoryAndStatusNamesTopOwner) {
  zetta::test::TempDir directory;
  zetta::NodeConfig config;
  config.data_dir = directory.path().string();
  config.preflight = false;
  zetta::Node node{config};

  testing::internal::CaptureStdout();
  ASSERT_TRUE(node.start());
  const std::string output = testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("zetta: INFO memory: rss "), std::string::npos);
  EXPECT_NE(node.status_line().find("| mem rss "), std::string::npos);
  EXPECT_NE(node.status_line().find(" top "), std::string::npos);
  const auto snapshot = node.memory_snapshot();
  EXPECT_EQ(snapshot.subsystems.size(), 8u);
  for (const std::string_view expected :
       {"audit", "pubsub", "replication", "datasets_tables",
        "registry_sessions_connections", "runtime", "models", "runs_jobs"}) {
    EXPECT_NE(std::find_if(snapshot.subsystems.begin(), snapshot.subsystems.end(),
                           [expected](const auto& subsystem) {
                             return subsystem.name == expected;
                           }),
              snapshot.subsystems.end());
  }
  node.stop();
}

} // namespace
} // namespace zetta::memory
