#include "zetta/pubsub/resources.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#include <sys/statvfs.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

#include "zetta/pubsub/log.hpp"

namespace zetta::pubsub {
namespace {

// cgroup v1 reports "unlimited" as a huge sentinel (e.g. 0x7FFFFFFFFFFFF000).
constexpr std::uint64_t kUnlimitedSentinel = std::uint64_t{1} << 60;

std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.remove_suffix(1);
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> result;
    for (auto& line : split(text, '\n')) {
        if (!line.empty()) result.push_back(std::move(line));
    }
    return result;
}

struct CgroupMount {
    std::filesystem::path mount_point;
    std::string root;  // the cgroup path the mount point shows
};

// Find the mount for cgroup v2 (fstype cgroup2) or the v1 hierarchy carrying
// `controller` (fstype cgroup, super options include it).
std::optional<CgroupMount> find_mount(std::string_view mountinfo, bool version_two,
                                      std::string_view controller) {
    for (const auto& line : lines(mountinfo)) {
        const auto dash = line.find(" - ");
        if (dash == std::string::npos) continue;
        const auto fields = split(std::string_view(line).substr(0, dash), ' ');
        const auto tail = split(std::string_view(line).substr(dash + 3), ' ');
        if (fields.size() < 5 || tail.size() < 3) continue;
        const std::string& filesystem_type = tail[0];
        if (version_two && filesystem_type == "cgroup2") {
            return CgroupMount{fields[4], fields[3]};
        }
        if (!version_two && filesystem_type == "cgroup") {
            for (const auto& option : split(tail[2], ',')) {
                if (option == controller) return CgroupMount{fields[4], fields[3]};
            }
        }
    }
    return std::nullopt;
}

// The process's cgroup path: v2 is the "0::" line; v1 the line whose
// controller list includes `controller`.
std::optional<std::string> own_cgroup(std::string_view cgroup_file, bool version_two,
                                      std::string_view controller) {
    for (const auto& line : lines(cgroup_file)) {
        const auto parts = split(line, ':');
        if (parts.size() < 3) continue;
        if (version_two && parts[0] == "0" && parts[1].empty()) return parts[2];
        if (!version_two) {
            for (const auto& name : split(parts[1], ',')) {
                if (name == controller) return parts[2];
            }
        }
    }
    return std::nullopt;
}

// Every directory from the process's cgroup up to the mount point, leaf first.
std::vector<std::filesystem::path> cgroup_chain(const CgroupMount& mount, std::string path) {
    if (mount.root != "/" && path.starts_with(mount.root)) path = path.substr(mount.root.size());
    std::vector<std::filesystem::path> chain;
    std::filesystem::path relative = std::filesystem::path(path).relative_path();
    while (true) {
        chain.push_back(mount.mount_point / relative);
        if (relative.empty()) break;
        relative = relative.parent_path();
    }
    return chain;
}

struct CgroupLimits {
    std::optional<DetectedAmount> memory;
    std::optional<double> cpus;
    std::string cpu_source;
};

CgroupLimits read_cgroup_limits(const ResourceProbe& probe) {
    CgroupLimits limits;
    const auto cgroup_file = probe.read_file(probe.proc_root / "self" / "cgroup");
    const auto mountinfo = probe.read_file(probe.proc_root / "self" / "mountinfo");
    if (!cgroup_file || !mountinfo) return limits;

    // cgroup v2: memory.max and cpu.max, tightest ancestor.
    if (auto mount = find_mount(*mountinfo, true, ""); mount) {
        if (auto path = own_cgroup(*cgroup_file, true, ""); path) {
            for (const auto& directory : cgroup_chain(*mount, *path)) {
                if (auto text = probe.read_file(directory / "memory.max"); text) {
                    if (auto value = parse_unsigned(*text);
                        value && (!limits.memory || *value < limits.memory->value)) {
                        limits.memory = DetectedAmount{
                            *value, "cgroup v2 memory.max " + directory.string()};
                    }
                }
                if (auto text = probe.read_file(directory / "cpu.max"); text) {
                    const auto parts = split(*text, ' ');
                    if (parts.size() >= 2) {
                        const auto quota = parse_unsigned(parts[0]);
                        const auto period = parse_unsigned(parts[1]);
                        if (quota && period && *period > 0) {
                            const double cpus = static_cast<double>(*quota) / static_cast<double>(*period);
                            if (!limits.cpus || cpus < *limits.cpus) {
                                limits.cpus = cpus;
                                std::string trimmed = *text;
                                while (!trimmed.empty() && trimmed.back() == '\n') trimmed.pop_back();
                                limits.cpu_source = "cgroup v2 cpu.max " + trimmed;
                            }
                        }
                    }
                }
            }
            return limits;
        }
    }

    // cgroup v1: memory.limit_in_bytes and cpu.cfs_quota_us / cfs_period_us.
    if (auto mount = find_mount(*mountinfo, false, "memory"); mount) {
        if (auto path = own_cgroup(*cgroup_file, false, "memory"); path) {
            for (const auto& directory : cgroup_chain(*mount, *path)) {
                if (auto text = probe.read_file(directory / "memory.limit_in_bytes"); text) {
                    if (auto value = parse_unsigned(*text); value && *value < kUnlimitedSentinel &&
                                                            (!limits.memory || *value < limits.memory->value)) {
                        limits.memory = DetectedAmount{
                            *value, "cgroup v1 memory.limit_in_bytes " + directory.string()};
                    }
                }
            }
        }
    }
    if (auto mount = find_mount(*mountinfo, false, "cpu"); mount) {
        if (auto path = own_cgroup(*cgroup_file, false, "cpu"); path) {
            for (const auto& directory : cgroup_chain(*mount, *path)) {
                const auto quota_text = probe.read_file(directory / "cpu.cfs_quota_us");
                const auto period_text = probe.read_file(directory / "cpu.cfs_period_us");
                if (!quota_text || !period_text) continue;
                if (quota_text->starts_with("-")) continue;  // no quota
                const auto quota = parse_unsigned(*quota_text);
                const auto period = parse_unsigned(*period_text);
                if (quota && period && *period > 0) {
                    const double cpus = static_cast<double>(*quota) / static_cast<double>(*period);
                    if (!limits.cpus || cpus < *limits.cpus) {
                        limits.cpus = cpus;
                        limits.cpu_source = "cgroup v1 cpu.cfs_quota_us " + std::to_string(*quota) +
                                            "/" + std::to_string(*period);
                    }
                }
            }
        }
    }
    return limits;
}

std::string format_cpus(double cpus) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3g", cpus);
    return buffer;
}

}  // namespace

std::string_view to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

LogSink stderr_log_sink() {
    return [](LogLevel level, std::string_view message) {
        std::fprintf(stderr, "pubsub: %.*s %.*s\n", static_cast<int>(to_string(level).size()),
                     to_string(level).data(), static_cast<int>(message.size()), message.data());
        std::fflush(stderr);
    };
}

std::string format_bytes(std::uint64_t bytes) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    if (unit == 0) {
        std::snprintf(buffer, sizeof buffer, "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1f %s", value, units[unit]);
    }
    return buffer;
}

ResourceProbe system_probe() {
    ResourceProbe probe;
    probe.read_file = [](const std::filesystem::path& path) -> std::optional<std::string> {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    probe.environment = [](std::string_view name) -> std::optional<std::string> {
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr) return std::nullopt;
        return std::string(value);
    };
    probe.host_memory_total = []() -> std::optional<std::uint64_t> {
#if defined(__APPLE__)
        std::uint64_t memsize = 0;
        std::size_t length = sizeof memsize;
        if (sysctlbyname("hw.memsize", &memsize, &length, nullptr, 0) != 0) return std::nullopt;
        return memsize;
#else
        std::ifstream in("/proc/meminfo");
        std::string key;
        std::uint64_t kilobytes = 0;
        std::string unit;
        while (in >> key >> kilobytes >> unit) {
            if (key == "MemTotal:") return kilobytes * 1024;
        }
        return std::nullopt;
#endif
    };
    probe.host_memory_available = []() -> std::optional<std::uint64_t> {
#if defined(__APPLE__)
        vm_statistics64_data_t statistics{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                              reinterpret_cast<host_info64_t>(&statistics), &count) != KERN_SUCCESS) {
            return std::nullopt;
        }
        vm_size_t page_size = 0;
        host_page_size(mach_host_self(), &page_size);
        return static_cast<std::uint64_t>(statistics.free_count + statistics.inactive_count) * page_size;
#else
        std::ifstream in("/proc/meminfo");
        std::string key;
        std::uint64_t kilobytes = 0;
        std::string unit;
        while (in >> key >> kilobytes >> unit) {
            if (key == "MemAvailable:") return kilobytes * 1024;
        }
        return std::nullopt;
#endif
    };
    probe.disk_free = [](const std::filesystem::path& path) -> std::optional<std::uint64_t> {
        std::filesystem::path existing = path;
        std::error_code error;
        while (!existing.empty() && !std::filesystem::exists(existing, error)) {
            existing = existing.parent_path();
        }
        if (existing.empty()) existing = ".";
        struct statvfs vfs {};
        if (statvfs(existing.c_str(), &vfs) != 0) return std::nullopt;
        return static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    };
    probe.hardware_threads = [] { return std::max(1U, std::thread::hardware_concurrency()); };
    return probe;
}

ResourceBudget detect_resources(const ResourceProbe& probe,
                                const std::filesystem::path& data_directory,
                                const ResourceOverrides& overrides) {
    ResourceBudget budget;

    // Memory: configuration wins; otherwise the tightest of cgroup, Downward
    // API limit and host.
    std::vector<DetectedAmount> candidates;
    const auto cgroup = read_cgroup_limits(probe);
    if (cgroup.memory) candidates.push_back(*cgroup.memory);
    if (auto text = probe.environment(kEnvMemoryLimit); text) {
        if (auto value = parse_unsigned(*text); value && *value > 0) {
            candidates.push_back({*value, "Downward API " + std::string(kEnvMemoryLimit)});
        }
    }
    if (auto text = probe.environment(kEnvMemoryRequest); text) {
        if (auto value = parse_unsigned(*text); value && *value > 0) {
            budget.memory_request = DetectedAmount{*value, "Downward API " + std::string(kEnvMemoryRequest)};
        }
    }
    std::optional<std::uint64_t> host_total = probe.host_memory_total ? probe.host_memory_total() : std::nullopt;
    if (host_total) candidates.push_back({*host_total, "host"});

    if (overrides.memory_bytes) {
        budget.memory = {*overrides.memory_bytes, "configuration"};
        for (const auto& candidate : candidates) {
            budget.memory_ignored.push_back(candidate.source + " " + format_bytes(candidate.value));
        }
    } else if (!candidates.empty()) {
        const auto tightest = std::ranges::min_element(
            candidates, {}, [](const DetectedAmount& amount) { return amount.value; });
        budget.memory = *tightest;
        for (const auto& candidate : candidates) {
            if (&candidate != &*tightest) {
                budget.memory_ignored.push_back(candidate.source + " " + format_bytes(candidate.value));
            }
        }
    } else {
        budget.memory = {std::uint64_t{1} << 30, "fallback (nothing detected)"};
    }
    if (probe.host_memory_available && budget.memory.source == "host") {
        if (auto available = probe.host_memory_available(); available && host_total) {
            budget.memory_used = DetectedAmount{*host_total - std::min(*host_total, *available), "host"};
        }
    }

    // Disk: free space on the data directory's filesystem.
    if (overrides.disk_free_bytes) {
        budget.disk_free = {*overrides.disk_free_bytes, "configuration"};
    } else if (auto free = probe.disk_free ? probe.disk_free(data_directory) : std::nullopt; free) {
        budget.disk_free = {*free, "statvfs " + data_directory.string()};
    } else {
        budget.disk_free = {0, "unknown (statvfs failed)"};
    }

    // CPU: configuration, then the cgroup quota, then the Downward API limit,
    // then hardware threads.
    const unsigned threads = probe.hardware_threads ? probe.hardware_threads() : 1U;
    if (overrides.cpus) {
        budget.cpus = *overrides.cpus;
        budget.cpu_source = "configuration";
    } else if (cgroup.cpus) {
        budget.cpus = *cgroup.cpus;
        budget.cpu_source = cgroup.cpu_source;
    } else if (auto text = probe.environment(kEnvCpuLimit); text && parse_unsigned(*text)) {
        budget.cpus = static_cast<double>(*parse_unsigned(*text)) / 1000.0;
        budget.cpu_source = "Downward API " + std::string(kEnvCpuLimit);
    } else {
        budget.cpus = threads;
        budget.cpu_source = "hardware threads";
    }
    if (budget.cpus <= 0) budget.cpus = 1;
    return budget;
}

PubSubLimits derive_limits(const ResourceBudget& budget, const LimitOverrides& overrides,
                           const LogSink& log) {
    PubSubLimits limits;
    const std::size_t whole_cpus =
        std::max<std::size_t>(1, static_cast<std::size_t>(budget.cpus + 0.999));
    limits.cpus = budget.cpus;
    limits.memory_bytes = overrides.memory_bytes.value_or(budget.memory.value / 4);
    limits.spill_disk_bytes = overrides.spill_disk_bytes.value_or(budget.disk_free.value / 2);
    limits.topics_per_thread = std::max<std::size_t>(1, overrides.topics_per_thread.value_or(250));
    limits.dispatch_min_threads =
        std::max<std::size_t>(1, overrides.dispatch_min_threads.value_or(1));
    limits.dispatch_max_threads = std::max(
        limits.dispatch_min_threads,
        overrides.dispatch_max_threads.value_or(whole_cpus));
    limits.hold_publisher = overrides.hold_publisher.value_or(limits.hold_publisher);
    limits.shutdown_budget = overrides.shutdown_budget.value_or(limits.shutdown_budget);

    if (!log) return limits;
    std::string memory_line = "memory budget " + format_bytes(budget.memory.value) + " (" +
                              budget.memory.source;
    for (const auto& ignored : budget.memory_ignored) memory_line += "; " + ignored + " ignored";
    memory_line += ")";
    log(LogLevel::Info, memory_line);
    if (budget.memory_request) {
        log(LogLevel::Info, "memory request " + format_bytes(budget.memory_request->value) + " (" +
                                budget.memory_request->source + ")");
    }
    log(LogLevel::Info,
        "memory for topics " + format_bytes(limits.memory_bytes) +
            (overrides.memory_bytes ? " (configuration)" : " (25% of budget; override: pubsub.memory)"));
    log(LogLevel::Info, "spill disk " + format_bytes(limits.spill_disk_bytes) +
                            (overrides.spill_disk_bytes
                                 ? std::string(" (configuration)")
                                 : " (50% of " + format_bytes(budget.disk_free.value) + " free; " +
                                       budget.disk_free.source + "; override: pubsub.spill_disk)"));
    log(LogLevel::Info,
        "cpu " + format_cpus(budget.cpus) + " (" + budget.cpu_source +
            "); dispatch pool one thread per " + std::to_string(limits.topics_per_thread) +
            " topics, minimum " + std::to_string(limits.dispatch_min_threads) +
            ", maximum " + std::to_string(limits.dispatch_max_threads) + " threads");
    return limits;
}

}  // namespace zetta::pubsub
