// SPDX-License-Identifier: MIT

#include "zetta/memory/accounting.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <sys/resource.h>

#if defined(__linux__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

namespace zetta::memory {
namespace {

std::uint64_t kibibytes_from_status(const std::string& wanted) {
#if defined(__linux__)
  std::ifstream input{"/proc/self/status"};
  const std::string prefix = wanted + ":";
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with(prefix)) {
      std::istringstream value_input{line.substr(prefix.size())};
      std::uint64_t value = 0;
      value_input >> value;
      return value * 1024;
    }
  }
#else
  (void)wanted;
#endif
  return 0;
}

std::uint64_t kibibytes_from_smaps(const std::string& wanted) {
#if defined(__linux__)
  std::ifstream input{"/proc/self/smaps_rollup"};
  const std::string prefix = wanted + ":";
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with(prefix)) {
      std::istringstream value_input{line.substr(prefix.size())};
      std::uint64_t value = 0;
      value_input >> value;
      return value * 1024;
    }
  }
#else
  (void)wanted;
#endif
  return 0;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

MemoryAccounting::MemoryAccounting(AllocatorReader allocator_reader)
    : allocator_reader_{std::move(allocator_reader)} {
  if (!allocator_reader_) {
    allocator_reader_ = read_allocator_sample;
  }
}

MemoryAccounting::ProbeId MemoryAccounting::register_probe(std::string subsystem,
                                                            Probe probe) {
  std::lock_guard lock{mutex_};
  const auto id = next_id_++;
  registrations_.push_back(Registration{id, std::move(subsystem), std::move(probe)});
  return id;
}

bool MemoryAccounting::unregister_probe(ProbeId id) {
  std::lock_guard lock{mutex_};
  const auto it = std::find_if(registrations_.begin(), registrations_.end(),
                               [id](const Registration& registration) {
                                 return registration.id == id;
                               });
  if (it == registrations_.end()) {
    return false;
  }
  registrations_.erase(it);
  return true;
}

Snapshot MemoryAccounting::snapshot() const {
  std::vector<Registration> registrations;
  {
    std::lock_guard lock{mutex_};
    registrations = registrations_;
  }

  Snapshot result;
  result.allocator = allocator_reader_();
  result.sampled_at_ms = now_ms();

  std::unordered_map<std::string, std::size_t> positions;
  for (const auto& registration : registrations) {
    const auto sample = registration.probe();
    const auto [it, inserted] = positions.emplace(registration.subsystem,
                                                   result.subsystems.size());
    if (inserted) {
      result.subsystems.push_back(SubsystemSample{registration.subsystem});
    }
    auto& subsystem = result.subsystems[it->second];
    subsystem.bytes += sample.bytes;
    subsystem.items += sample.items;
    subsystem.reserved_bytes += sample.reserved_bytes;
  }

  for (auto& subsystem : result.subsystems) {
    result.attributed_bytes += subsystem.bytes;
    if (result.allocator.in_use_bytes != 0) {
      subsystem.share_of_in_use =
          100.0 * static_cast<double>(subsystem.bytes) /
          static_cast<double>(result.allocator.in_use_bytes);
    }
  }
  if (result.attributed_bytes <= result.allocator.in_use_bytes) {
    result.unattributed_bytes = result.allocator.in_use_bytes - result.attributed_bytes;
  } else {
    result.over_attributed_bytes = result.attributed_bytes - result.allocator.in_use_bytes;
  }
  std::sort(result.subsystems.begin(), result.subsystems.end(),
            [](const SubsystemSample& left, const SubsystemSample& right) {
              if (left.bytes == right.bytes) {
                return left.name < right.name;
              }
              return left.bytes > right.bytes;
            });
  return result;
}

AllocatorSample read_allocator_sample() {
  AllocatorSample sample;
#if defined(__linux__)
  const auto info = mallinfo2();
  sample.allocator_available = true;
  sample.in_use_bytes = static_cast<std::uint64_t>(info.uordblks) +
                        static_cast<std::uint64_t>(info.hblkhd);
  sample.free_held_bytes = static_cast<std::uint64_t>(info.fordblks);
  sample.rss_bytes = kibibytes_from_status("VmRSS");
  sample.peak_rss_bytes = kibibytes_from_status("VmHWM");
  sample.anonymous_bytes = kibibytes_from_smaps("Anonymous");
  sample.rss_available = sample.rss_bytes != 0;
#elif defined(__APPLE__)
  vm_address_t* zones = nullptr;
  unsigned int zone_count = 0;
  const auto zones_result = malloc_get_all_zones(
      mach_task_self(), nullptr, &zones, &zone_count);
  if (zones_result == KERN_SUCCESS) {
    sample.allocator_available = true;
    for (unsigned int index = 0; index < zone_count; ++index) {
      malloc_statistics_t statistics{};
      malloc_zone_statistics(reinterpret_cast<malloc_zone_t*>(zones[index]),
                             &statistics);
      sample.in_use_bytes += statistics.size_in_use;
      if (statistics.size_allocated > statistics.size_in_use) {
        sample.free_held_bytes += statistics.size_allocated - statistics.size_in_use;
      }
    }
  }

  mach_task_basic_info_data_t basic{};
  mach_msg_type_number_t basic_count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&basic), &basic_count) == KERN_SUCCESS) {
    sample.rss_available = true;
    sample.rss_bytes = basic.resident_size;
  }
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    sample.peak_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
  }
#endif
  return sample;
}

std::string format_bytes(std::uint64_t bytes) {
  constexpr std::uint64_t kKib = 1024;
  constexpr std::uint64_t kMib = kKib * 1024;
  constexpr std::uint64_t kGib = kMib * 1024;
  std::ostringstream output;
  if (bytes >= kGib) {
    output << std::fixed << std::setprecision(1)
           << static_cast<double>(bytes) / static_cast<double>(kGib) << 'G';
  } else if (bytes >= kMib) {
    output << (bytes / kMib) << 'M';
  } else if (bytes >= kKib) {
    output << (bytes / kKib) << 'K';
  } else {
    output << bytes << 'B';
  }
  return output.str();
}

std::string format_log_line(const Snapshot& snapshot) {
  std::ostringstream output;
  output << "memory: rss " << format_bytes(snapshot.allocator.rss_bytes)
         << " in-use " << format_bytes(snapshot.allocator.in_use_bytes);
  for (const auto& subsystem : snapshot.subsystems) {
    output << ' ' << subsystem.name << ' ' << format_bytes(subsystem.bytes);
  }
  output << " unattributed " << format_bytes(snapshot.unattributed_bytes)
         << " free-held " << format_bytes(snapshot.allocator.free_held_bytes);
  return output.str();
}

TrimResult trim_allocator() {
#if defined(__linux__)
  return TrimResult{true, malloc_trim(0) != 0};
#else
  return TrimResult{};
#endif
}

} // namespace zetta::memory
