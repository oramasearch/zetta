// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace zetta::memory {

struct ProbeSample {
  std::uint64_t bytes{0};
  std::uint64_t items{0};
  std::uint64_t reserved_bytes{0};
};

struct AllocatorSample {
  bool allocator_available{false};
  bool rss_available{false};
  std::uint64_t in_use_bytes{0};
  std::uint64_t free_held_bytes{0};
  std::uint64_t rss_bytes{0};
  std::uint64_t peak_rss_bytes{0};
  std::uint64_t anonymous_bytes{0};
};

struct SubsystemSample {
  std::string name;
  std::uint64_t bytes{0};
  std::uint64_t items{0};
  std::uint64_t reserved_bytes{0};
  double share_of_in_use{0.0};
};

struct Snapshot {
  AllocatorSample allocator;
  std::vector<SubsystemSample> subsystems;
  std::uint64_t attributed_bytes{0};
  std::uint64_t unattributed_bytes{0};
  std::uint64_t over_attributed_bytes{0};
  std::int64_t sampled_at_ms{0};
};

using Probe = std::function<ProbeSample()>;
using AllocatorReader = std::function<AllocatorSample()>;

class MemoryAccounting {
public:
  using ProbeId = std::uint64_t;

  explicit MemoryAccounting(AllocatorReader allocator_reader = {});

  ProbeId register_probe(std::string subsystem, Probe probe);
  bool unregister_probe(ProbeId id);
  [[nodiscard]] Snapshot snapshot() const;

private:
  struct Registration {
    ProbeId id;
    std::string subsystem;
    Probe probe;
  };

  mutable std::mutex mutex_;
  std::vector<Registration> registrations_;
  ProbeId next_id_{1};
  AllocatorReader allocator_reader_;
};

[[nodiscard]] AllocatorSample read_allocator_sample();
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_log_line(const Snapshot& snapshot);

struct TrimResult {
  bool supported{false};
  bool released{false};
};

[[nodiscard]] TrimResult trim_allocator();

} // namespace zetta::memory
