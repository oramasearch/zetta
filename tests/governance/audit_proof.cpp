// Native process-isolated audit replication/RSS proof for #1164.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"

namespace {

std::uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t information{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&information), &count) != KERN_SUCCESS) {
        return 0;
    }
    return information.resident_size;
#else
    std::ifstream status("/proc/self/statm");
    std::uint64_t total = 0;
    std::uint64_t resident = 0;
    status >> total >> resident;
    return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

std::uint64_t peak_bytes() {
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return usage.ru_maxrss;
#else
    return usage.ru_maxrss * 1024ULL;
#endif
}

std::string identity(unsigned ordinal) {
    return ordinal == 0 ? "11640000-0000-4000-8000-000000000001"
                        : "11640000-0000-4000-8000-000000000002";
}

bool complete(const zetta::Node& node) {
    if (node.audit_event_count() != 1'000'000) {
        return false;
    }
    const auto watermarks = node.audit_watermarks();
    for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
        if (!watermarks.contains(identity(ordinal)) ||
            watermarks.at(identity(ordinal)).at("high") != 500'000) {
            return false;
        }
    }
    return true;
}

void release_allocator_memory() {
#if defined(__APPLE__)
    ::malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
    ::malloc_trim(0);
#endif
}

void pause_for_profile(const std::string& phase, unsigned ordinal,
                       const zetta::Node& node) {
    const char* requested = std::getenv("ZETTA_AUDIT_PROOF_PAUSE");
    if (!requested || phase != requested) {
        return;
    }
    std::cout << "#1164 profile pid=" << ::getpid() << " node=" << ordinal
              << " phase=" << phase << " resident=" << resident_bytes()
              << " audit_memory=" << node.audit_memory_bytes() << std::endl;
    ::raise(SIGSTOP);
}

void write_local_events(zetta::Node& node) {
    zetta::governance::AuditEventValue value;
    value.actor = "service:replicated-proof";
    value.target = "api:get_topic";
    value.detail = "replicated native audit load";
    for (std::size_t index = 0; index < 500'000; ++index) {
        node.record_audit_event(value);
        if (index % 512 == 0) {
            node.tick();
        }
    }
}

nlohmann::json measure_report(zetta::Node& node,
                              const zetta::cluster::ReplicationManager& replication,
                              const std::string& phase, unsigned ordinal,
                              std::uint64_t steady_baseline, std::uint64_t startup_extra) {
    const auto steady_resident = resident_bytes();
    const auto steady_extra = steady_resident - std::min(steady_resident, steady_baseline);
    pause_for_profile(phase, ordinal, node);
    release_allocator_memory();
    const auto relieved_resident = resident_bytes();
    const auto relieved_extra = relieved_resident - std::min(relieved_resident, steady_baseline);
    const bool clean_catalog = std::ranges::none_of(replication.snapshot().items(),
        [](const auto& item) {
            return item.key().starts_with("audit:");
        });
    const auto accounting = node.memory_snapshot();
    const auto audit = std::ranges::find_if(accounting.subsystems, [](const auto& sample) {
        return sample.name == "audit";
    });
    const auto audit_items = audit == accounting.subsystems.end() ? 0 : audit->items;
    const bool accounted = audit != accounting.subsystems.end() &&
        audit_items == node.audit_event_count() && audit->bytes == node.audit_memory_bytes();
    const bool bounded = startup_extra < 150ULL * 1024 * 1024 &&
        steady_extra < 100ULL * 1024 * 1024 && clean_catalog && accounted;
    std::cout << "#1164 " << phase << " node=" << ordinal
              << " events=" << node.audit_event_count()
              << " steady_rss_extra=" << steady_extra
              << " relieved_rss_extra=" << relieved_extra
              << " startup_peak_extra=" << startup_extra
              << " audit_memory=" << node.audit_memory_bytes()
              << " audit_items=" << audit_items
              << " catalog_bytes=" << replication.save().size() << std::endl;
    return {{"events", node.audit_event_count()}, {"steady_rss_extra", steady_extra},
            {"relieved_rss_extra", relieved_extra},
            {"startup_peak_extra", startup_extra}, {"ok", bounded}};
}

bool exchange_reports(zetta::Node& node, const std::filesystem::path& root,
                       const std::string& phase, unsigned ordinal,
                       const nlohmann::json& report,
                       std::chrono::steady_clock::time_point deadline) {
    const auto report_path = root / (phase + "-" + std::to_string(ordinal) + ".json");
    const auto temporary = report_path.string() + ".tmp";
    {
        std::ofstream output(temporary);
        output << report.dump();
    }
    std::filesystem::rename(temporary, report_path);
    const auto other_report = root / (phase + "-" + std::to_string(1 - ordinal) + ".json");
    while (!std::filesystem::exists(other_report) && std::chrono::steady_clock::now() < deadline) {
        node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::filesystem::exists(other_report);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    const unsigned ordinal = static_cast<unsigned>(std::stoul(argv[2]));
    const auto port = static_cast<std::uint16_t>(std::stoul(argv[3]));
    const auto seed_port = static_cast<std::uint16_t>(std::stoul(argv[4]));
    const std::string phase(argv[5]);
    zetta::cluster::ReplicationManager replication(identity(ordinal));
    zetta::NodeConfig config;
    config.data_dir = (root / ("node-" + std::to_string(ordinal))).string();
    config.preflight = false;
    config.audit_retention = std::chrono::hours(0);
    zetta::Node node(config, &replication);
    const auto startup_baseline = resident_bytes();
    if (!node.start()) {
        return 3;
    }
    const auto startup_extra = peak_bytes() - std::min(peak_bytes(), startup_baseline);
    zetta::cluster::ClusterConfig cluster_config;
    cluster_config.listen_host = "127.0.0.1";
    cluster_config.listen_port = port;
    if (ordinal != 0) {
        cluster_config.seeds = {"127.0.0.1:" + std::to_string(seed_port)};
    }
    zetta::cluster::Cluster cluster(identity(ordinal), cluster_config, &replication);
    cluster.start();
    if (!cluster.is_listening()) {
        return 4;
    }
    node.attach_cluster(cluster, 1);
    node.tick();
    const auto steady_baseline = resident_bytes();
    if (phase == "load") {
        write_local_events(node);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (!complete(node) && std::chrono::steady_clock::now() < deadline) {
        node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!complete(node)) {
        std::cerr << "#1164 convergence failed: node=" << ordinal
                  << " events=" << node.audit_event_count()
                  << " watermarks=" << node.audit_watermarks() << '\n';
        node.stop();
        cluster.stop();
        return 5;
    }
    const auto report = measure_report(node, replication, phase, ordinal, steady_baseline, startup_extra);
    const bool peer_reported = exchange_reports(node, root, phase, ordinal, report, deadline);
    node.stop();
    cluster.stop();
    return report.at("ok").get<bool>() && peer_reported ? 0 : 6;
}
