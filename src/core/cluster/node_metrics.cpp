#include "zetta/cluster/node_metrics.hpp"

#include <sys/statvfs.h>

#include <cstdint>
#include <string>
#include <thread>

#include "zetta/util/timestamp.hpp"

#if defined(__APPLE__)
#include <ifaddrs.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <cstring>
#elif defined(__linux__)
#include <cstdio>
#include <cstring>
#endif

namespace zetta::cluster {
namespace {

// --- Per-platform raw counter reads ------------------------------------------
//
// Each returns true and fills its out-params when the host can report that
// counter, false (leaving the out-params untouched) when it can't — the
// graceful-degradation contract. None throw.

// Total + non-idle CPU ticks across all logical cores. CPU utilization is the
// delta of `busy` over the delta of `total` between two reads.
bool read_cpu_ticks(std::uint64_t& total, std::uint64_t& busy) {
#if defined(__APPLE__)
    host_cpu_load_info_data_t info{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&info),
                        &count) != KERN_SUCCESS) {
        return false;
    }
    const std::uint64_t user = info.cpu_ticks[CPU_STATE_USER];
    const std::uint64_t system = info.cpu_ticks[CPU_STATE_SYSTEM];
    const std::uint64_t idle = info.cpu_ticks[CPU_STATE_IDLE];
    const std::uint64_t nice = info.cpu_ticks[CPU_STATE_NICE];
    total = user + system + idle + nice;
    busy = user + system + nice;
    return true;
#elif defined(__linux__)
    std::FILE* file = std::fopen("/proc/stat", "re");
    if (!file) return false;
    char label[16] = {};
    std::uint64_t values[10] = {};
    // "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    const int read = std::fscanf(
        file, "%15s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", label,
        reinterpret_cast<unsigned long long*>(&values[0]),
        reinterpret_cast<unsigned long long*>(&values[1]),
        reinterpret_cast<unsigned long long*>(&values[2]),
        reinterpret_cast<unsigned long long*>(&values[3]),
        reinterpret_cast<unsigned long long*>(&values[4]),
        reinterpret_cast<unsigned long long*>(&values[5]),
        reinterpret_cast<unsigned long long*>(&values[6]),
        reinterpret_cast<unsigned long long*>(&values[7]),
        reinterpret_cast<unsigned long long*>(&values[8]),
        reinterpret_cast<unsigned long long*>(&values[9]));
    std::fclose(file);
    if (read < 5 || std::strcmp(label, "cpu") != 0) return false;
    std::uint64_t sum = 0;
    for (std::uint64_t value : values) sum += value;
    total = sum;
    const std::uint64_t idle = values[3] + values[4];  // idle + iowait
    busy = sum - idle;
    return true;
#else
    (void)total;
    (void)busy;
    return false;
#endif
}

// Physical RAM used + total, in bytes (used = total − available).
bool read_memory(std::uint64_t& used, std::uint64_t& total) {
#if defined(__APPLE__)
    std::uint64_t memsize = 0;
    std::size_t length = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &length, nullptr, 0) != 0) {
        return false;
    }
    vm_size_t page_size = 0;
    if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS) {
        return false;
    }
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm),
                          &count) != KERN_SUCCESS) {
        return false;
    }
    // Used ≈ active + wired + compressed (the "memory used" Activity Monitor
    // shows); inactive + free + speculative are reclaimable / available.
    const std::uint64_t used_pages = static_cast<std::uint64_t>(vm.active_count) +
                                     vm.wire_count + vm.compressor_page_count;
    total = memsize;
    used = used_pages * static_cast<std::uint64_t>(page_size);
    if (used > total) used = total;
    return true;
#elif defined(__linux__)
    std::FILE* file = std::fopen("/proc/meminfo", "re");
    if (!file) return false;
    std::uint64_t total_kb = 0;
    std::uint64_t available_kb = 0;
    bool have_total = false;
    bool have_available = false;
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long value = 0;
        if (std::sscanf(line, "MemTotal: %llu kB", &value) == 1) {
            total_kb = value;
            have_total = true;
        } else if (std::sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
            available_kb = value;
            have_available = true;
        }
        if (have_total && have_available) break;
    }
    std::fclose(file);
    if (!have_total || !have_available) return false;
    total = total_kb * 1024ULL;
    const std::uint64_t available = available_kb * 1024ULL;
    used = available <= total ? total - available : 0;
    return true;
#else
    (void)used;
    (void)total;
    return false;
#endif
}

// Cumulative received + transmitted bytes across all non-loopback interfaces.
// Network rates are the delta of these over wall-clock time.
bool read_network_bytes(std::uint64_t& rx, std::uint64_t& tx) {
#if defined(__APPLE__)
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0 || !addresses) return false;
    std::uint64_t rx_total = 0;
    std::uint64_t tx_total = 0;
    for (ifaddrs* ifa = addresses; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        if (ifa->ifa_name && std::strncmp(ifa->ifa_name, "lo", 2) == 0) continue;
        if (!ifa->ifa_data) continue;
        const auto* data = static_cast<const if_data*>(ifa->ifa_data);
        rx_total += data->ifi_ibytes;
        tx_total += data->ifi_obytes;
    }
    freeifaddrs(addresses);
    rx = rx_total;
    tx = tx_total;
    return true;
#elif defined(__linux__)
    std::FILE* file = std::fopen("/proc/net/dev", "re");
    if (!file) return false;
    char line[512];
    // Skip the two header lines.
    if (!std::fgets(line, sizeof(line), file) ||
        !std::fgets(line, sizeof(line), file)) {
        std::fclose(file);
        return false;
    }
    std::uint64_t rx_total = 0;
    std::uint64_t tx_total = 0;
    while (std::fgets(line, sizeof(line), file)) {
        char* colon = std::strchr(line, ':');
        if (!colon) continue;
        char name[64] = {};
        std::size_t name_len = static_cast<std::size_t>(colon - line);
        while (name_len > 0 && line[name_len - 1] == ' ') --name_len;
        std::size_t start = 0;
        while (start < name_len && line[start] == ' ') ++start;
        const std::size_t copy_len =
            name_len - start < sizeof(name) - 1 ? name_len - start : sizeof(name) - 1;
        std::memcpy(name, line + start, copy_len);
        if (std::strcmp(name, "lo") == 0) continue;
        unsigned long long rx_bytes = 0;
        unsigned long long tx_bytes = 0;
        // rx: bytes packets errs drop fifo frame compressed multicast (8)
        // tx: bytes packets ... (the 9th number is tx bytes)
        unsigned long long ignore = 0;
        if (std::sscanf(colon + 1,
                        "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &rx_bytes, &ignore, &ignore, &ignore, &ignore, &ignore,
                        &ignore, &ignore, &tx_bytes) == 9) {
            rx_total += rx_bytes;
            tx_total += tx_bytes;
        }
    }
    std::fclose(file);
    rx = rx_total;
    tx = tx_total;
    return true;
#else
    (void)rx;
    (void)tx;
    return false;
#endif
}

// Used + total bytes of the filesystem backing `path` (POSIX statvfs, both
// platforms). used = (blocks − free) × fragment size.
bool read_storage(const std::string& path, std::uint64_t& used,
                  std::uint64_t& total) {
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) != 0) return false;
    const std::uint64_t frag = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    total = static_cast<std::uint64_t>(vfs.f_blocks) * frag;
    const std::uint64_t free_bytes =
        static_cast<std::uint64_t>(vfs.f_bfree) * frag;
    used = free_bytes <= total ? total - free_bytes : 0;
    return total > 0;
}

double percent_of(std::uint64_t used, std::uint64_t total) {
    if (total == 0) return 0.0;
    const double value = (static_cast<double>(used) / static_cast<double>(total)) * 100.0;
    if (value < 0.0) return 0.0;
    if (value > 100.0) return 100.0;
    return value;
}

}  // namespace

NodeMetricsSampler::NodeMetricsSampler() {
    // Seed a baseline so the first sample() after any real elapsed time already
    // has an interval to measure the CPU + network rates over.
    if (read_cpu_ticks(prev_cpu_total_, prev_cpu_busy_)) {
        have_cpu_baseline_ = true;
    }
    if (read_network_bytes(prev_net_rx_, prev_net_tx_)) {
        prev_net_time_ = std::chrono::steady_clock::now();
        have_net_baseline_ = true;
    }
}

NodeMetrics NodeMetricsSampler::sample(const std::string& storage_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    NodeMetrics metrics;
    metrics.sampled_at = zetta::util::now_iso8601();

    metrics.cpu_cores = std::thread::hardware_concurrency();

    // CPU: a delta measurement over the previous reading.
    std::uint64_t cpu_total = 0;
    std::uint64_t cpu_busy = 0;
    if (read_cpu_ticks(cpu_total, cpu_busy)) {
        if (have_cpu_baseline_ && cpu_total > prev_cpu_total_) {
            const std::uint64_t total_delta = cpu_total - prev_cpu_total_;
            const std::uint64_t busy_delta =
                cpu_busy >= prev_cpu_busy_ ? cpu_busy - prev_cpu_busy_ : 0;
            metrics.cpu_percent = percent_of(busy_delta, total_delta);
            metrics.cpu_percent_available = true;
        }
        prev_cpu_total_ = cpu_total;
        prev_cpu_busy_ = cpu_busy;
        have_cpu_baseline_ = true;
    }

    // Memory.
    if (read_memory(metrics.memory_used_bytes, metrics.memory_total_bytes)) {
        metrics.memory_percent =
            percent_of(metrics.memory_used_bytes, metrics.memory_total_bytes);
        metrics.memory_available = true;
    }

    // GPU + VRAM: absent in zetta-core (no CUDA/NVML may leak into the core) —
    // gpu_available stays false; a future accelerator-telemetry binding fills it.

    // Storage (the node's data_dir filesystem).
    if (read_storage(storage_path, metrics.storage_used_bytes,
                     metrics.storage_total_bytes)) {
        metrics.storage_percent =
            percent_of(metrics.storage_used_bytes, metrics.storage_total_bytes);
        metrics.storage_available = true;
    }

    // Network: rx/tx rates over the previous reading.
    std::uint64_t net_rx = 0;
    std::uint64_t net_tx = 0;
    if (read_network_bytes(net_rx, net_tx)) {
        const auto now = std::chrono::steady_clock::now();
        if (have_net_baseline_) {
            const double seconds =
                std::chrono::duration<double>(now - prev_net_time_).count();
            if (seconds > 0.0) {
                const std::uint64_t rx_delta =
                    net_rx >= prev_net_rx_ ? net_rx - prev_net_rx_ : 0;
                const std::uint64_t tx_delta =
                    net_tx >= prev_net_tx_ ? net_tx - prev_net_tx_ : 0;
                metrics.net_rx_bytes_per_sec =
                    static_cast<std::uint64_t>(static_cast<double>(rx_delta) / seconds);
                metrics.net_tx_bytes_per_sec =
                    static_cast<std::uint64_t>(static_cast<double>(tx_delta) / seconds);
                metrics.network_available = true;
            }
        }
        prev_net_rx_ = net_rx;
        prev_net_tx_ = net_tx;
        prev_net_time_ = now;
        have_net_baseline_ = true;
    }
    // net_percent (link utilization) needs a capacity reference that isn't
    // reliably available cross-platform — leave it unavailable for now.

    return metrics;
}

}  // namespace zetta::cluster