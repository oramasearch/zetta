// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/pubsub/log.hpp"

// Detected defaults (#1030; decisions D10, D11 in docs/plans/pubsub/decisions.md).
// See docs/pubsub/limits.md.
//
// Pub/sub never guesses a limit: it detects the node's budget from the
// environment — cgroup v2 / v1 limits (tightest ancestor), Kubernetes
// requests/limits exposed through Downward API environment variables, the
// host's memory, free disk on the data directory — derives its defaults as
// fractions of that budget, lets configuration override any value, and logs
// every input, where it came from, and what was chosen.
namespace zetta::pubsub {

// Everything detection reads, injectable so tests fake a pod or a host.
struct ResourceProbe {
    std::function<std::optional<std::string>(const std::filesystem::path&)> read_file;
    std::function<std::optional<std::string>(std::string_view)> environment;
    std::function<std::optional<std::uint64_t>()> host_memory_total;
    std::function<std::optional<std::uint64_t>()> host_memory_available;
    std::function<std::optional<std::uint64_t>(const std::filesystem::path&)> disk_free;
    std::function<unsigned()> hardware_threads;
    std::filesystem::path proc_root = "/proc";  // where self/cgroup and self/mountinfo live
};

// The real machine: /proc, the process environment, sysctl / meminfo, statvfs.
ResourceProbe system_probe();

// Downward API environment variables the orama-operator sets on a Zetta pod.
inline constexpr std::string_view kEnvMemoryLimit = "ZETTA_POD_MEMORY_LIMIT";      // bytes
inline constexpr std::string_view kEnvMemoryRequest = "ZETTA_POD_MEMORY_REQUEST";  // bytes
inline constexpr std::string_view kEnvCpuLimit = "ZETTA_POD_CPU_LIMIT";            // millicores

struct DetectedAmount {
    std::uint64_t value = 0;
    std::string source;  // e.g. "cgroup v2 memory.max /kubepods/pod1/zetta"
};

struct ResourceBudget {
    DetectedAmount memory;                      // the tightest memory limit found
    std::optional<DetectedAmount> memory_used;  // current use, when known
    std::vector<std::string> memory_ignored;    // looser candidates, for the log
    DetectedAmount disk_free;                   // free bytes on the data directory
    double cpus = 1.0;                          // quota or hardware threads
    std::string cpu_source;
    std::optional<DetectedAmount> memory_request;  // Kubernetes request, when exposed
};

struct ResourceOverrides {
    std::optional<std::uint64_t> memory_bytes;
    std::optional<std::uint64_t> disk_free_bytes;
    std::optional<double> cpus;
};

ResourceBudget detect_resources(const ResourceProbe& probe,
                                const std::filesystem::path& data_directory,
                                const ResourceOverrides& overrides = {});

// Pub/sub's limits, derived from the budget. Every field is overridable.
struct PubSubLimits {
    std::uint64_t memory_bytes = 0;      // all topics on the node together
    std::uint64_t spill_disk_bytes = 0;  // spill files on the node together
    double cpus = 1.0;
    std::size_t topics_per_thread = 250;
    std::size_t dispatch_min_threads = 1;
    std::size_t dispatch_max_threads = 1;
    std::chrono::milliseconds hold_publisher{5'000};
    std::chrono::milliseconds shutdown_budget{25'000};
};

struct LimitOverrides {
    std::optional<std::uint64_t> memory_bytes;
    std::optional<std::uint64_t> spill_disk_bytes;
    std::optional<std::size_t> topics_per_thread;
    std::optional<std::size_t> dispatch_min_threads;
    std::optional<std::size_t> dispatch_max_threads;
    std::optional<std::chrono::milliseconds> hold_publisher;
    std::optional<std::chrono::milliseconds> shutdown_budget;
};

// Proposed fractions (decision D11): pub/sub memory = 25% of the memory
// budget and spill disk = 50% of free space. Dispatch defaults to one shared
// worker per 250 active keys, minimum one and maximum detected CPUs (#1157).
// Logs one line per input and per derived value.
PubSubLimits derive_limits(const ResourceBudget& budget, const LimitOverrides& overrides,
                           const LogSink& log);

// "4.0 GiB", "512 MiB", "12 KiB", "900 B".
std::string format_bytes(std::uint64_t bytes);

}  // namespace zetta::pubsub
