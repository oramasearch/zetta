// The object-store soak (#1005): a liquid-shaped load on a running node — packs
// written as appends and sealed with a barrier, index runs put whole, the oldest
// packs removed so the live set stays fixed — with a reader beside it, while the
// owner loop ticks (maintenance and compaction run on the pool). It fails if disk
// or memory follow the write history instead of the live set, or reads slow down.
// ZETTA_OBJECT_SOAK_BYTES sets how many bytes the writers append (default 2 GiB).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

namespace {

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) return 0.0;
    std::ranges::sort(samples);
    return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1))];
}

std::uint64_t path_bytes(const std::filesystem::path& path) {
    std::error_code error;
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path, error)) {
        if (entry.is_regular_file(error)) total += entry.file_size(error);
    }
    return total;
}

// The packs a writer has sealed, for the reader to read and the writer to retire.
class LivePacks {
public:
    void add(std::string key) {
        const std::lock_guard lock(mutex_);
        keys_.push_back(std::move(key));
    }
    std::optional<std::string> oldest_beyond(std::size_t keep) {
        const std::lock_guard lock(mutex_);
        if (keys_.size() <= keep) return std::nullopt;
        std::string key = std::move(keys_.front());
        keys_.pop_front();
        return key;
    }
    std::optional<std::string> pick(std::mt19937_64& random) {
        const std::lock_guard lock(mutex_);
        if (keys_.empty()) return std::nullopt;
        return keys_[random() % keys_.size()];
    }

private:
    std::mutex mutex_;
    std::deque<std::string> keys_;
};

}  // namespace

TEST(ObjectStoreSoak, DiskAndMemoryFollowTheLiveSetUnderSteadyChurn) {
    std::uint64_t total_bytes = 2ull * 1024 * 1024 * 1024;
    if (const char* configured = std::getenv("ZETTA_OBJECT_SOAK_BYTES")) {
        total_bytes = std::strtoull(configured, nullptr, 10);
    }
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication};
    ASSERT_TRUE(node.start());
    zetta::data::ObjectStore& store = *node.object_store();
    zetta::data::ObjectBucket bucket;
    bucket.name = "lake_ws";
    ASSERT_TRUE(store.create_bucket(bucket).ok());
    const std::size_t catalogue_before = replication.shared_snapshot()->dump().size();

    constexpr int kWriters = 4;
    constexpr std::size_t kPackBytes = 4 * 1024 * 1024;
    constexpr std::size_t kAppendBytes = 256 * 1024;
    constexpr std::size_t kPacksKept = 6;  // per writer: a live set of ~96 MiB of packs
    std::string slice(kAppendBytes, '\0');
    for (std::size_t index = 0; index < slice.size(); ++index) slice[index] = static_cast<char>(index * 31);

    std::atomic<std::uint64_t> appended{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> writers_done{false};
    std::vector<LivePacks> packs(kWriters);
    std::vector<std::thread> threads;
    for (int writer = 0; writer < kWriters; ++writer) {
        threads.emplace_back([&, writer] {
            for (std::uint64_t pack = 0; appended < total_bytes && !failed; ++pack) {
                const std::string key = "packs/" + std::to_string(writer) + "-" + std::to_string(pack);
                for (std::size_t offset = 0; offset < kPackBytes; offset += kAppendBytes) {
                    if (!store.put_range("lake_ws", key, offset, slice, {}).ok()) failed = true;
                    appended += kAppendBytes;
                }
                // Seal: durable pack, then its index run and the store marker.
                if (!store.barrier("lake_ws").ok()) failed = true;
                if (!store.put("lake_ws", "index/" + std::to_string(writer) + "-" + std::to_string(pack),
                               std::string(8 * 1024, 'i'), {})
                         .ok() ||
                    !store.put("lake_ws", "LIQUID_STORE", "generation " + std::to_string(pack), {}).ok()) {
                    failed = true;
                }
                packs[static_cast<std::size_t>(writer)].add(key);
                // Garbage collection: the oldest packs and their index runs go.
                while (const auto old = packs[static_cast<std::size_t>(writer)].oldest_beyond(kPacksKept)) {
                    if (!store.remove("lake_ws", *old, {}).ok()) failed = true;
                    const std::string run = "index/" + old->substr(6);
                    if (!store.remove("lake_ws", run, {}).ok()) failed = true;
                }
            }
        });
    }
    std::vector<double> reads;
    std::mutex reads_mutex;
    std::thread reader([&] {
        std::mt19937_64 random(9);
        std::string bytes;
        while (!writers_done) {
            const auto key = packs[random() % kWriters].pick(random);
            if (!key) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const std::uint64_t offset = random() % (kPackBytes - 65536);
            const auto started = std::chrono::steady_clock::now();
            std::shared_ptr<zetta::data::ObjectReader> handle;
            const auto opened = store.open_read("lake_ws", *key, offset, 65536, handle);
            // A pack retired meanwhile is gone (404); anything else must read.
            if (opened.ok()) {
                if (!handle->read(0, 65536, bytes) ||
                    std::memcmp(bytes.data(), slice.data() + offset % kAppendBytes,
                                std::min<std::size_t>(65536, kAppendBytes - offset % kAppendBytes)) != 0) {
                    failed = true;
                }
                const double elapsed = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - started)
                                           .count();
                const std::lock_guard lock(reads_mutex);
                reads.push_back(elapsed);
            } else if (opened.status != zetta::data::ObjectStatus::NotFound) {
                failed = true;
            }
        }
    });

    // This thread is the owner loop: it ticks, which runs the object store's
    // upkeep (maintenance and a bounded compaction step) on the pool, and reports.
    std::uint64_t peak_disk = 0;
    std::uint64_t peak_rss = 0;
    std::uint64_t peak_directory = 0;
    // The process may carry other tests' memory: bound what this load adds.
    const std::uint64_t starting_rss = node.memory_snapshot().allocator.rss_bytes;
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t next_report = 0;
    const auto report = [&] {
        const auto stats = store.storage_stats();
        const auto memory = node.memory_snapshot();
        const std::uint64_t disk = path_bytes(dir.path() / "objects");
        peak_disk = std::max(peak_disk, disk);
        peak_rss = std::max(peak_rss, memory.allocator.rss_bytes);
        peak_directory = std::max(peak_directory, stats.memory_bytes);
        std::printf("[object soak] %6.0f MiB appended %5.1f s | objects %5llu live %6.1f MiB | "
                    "disk %6.1f MiB (frames live %6.1f MiB) in %3llu segments | directory %5.1f KiB | "
                    "RSS %6.1f MiB\n",
                    appended.load() / 1048576.0,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                    static_cast<unsigned long long>(stats.objects), stats.object_bytes / 1048576.0,
                    disk / 1048576.0, stats.live_bytes / 1048576.0,
                    static_cast<unsigned long long>(stats.segments),
                    stats.memory_bytes / 1024.0, memory.allocator.rss_bytes / 1048576.0);
        std::fflush(stdout);
    };
    while (appended < total_bytes && !failed) {
        node.tick();
        if (appended >= next_report) {
            report();
            next_report += total_bytes / 8;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& thread : threads) thread.join();
    writers_done = true;
    reader.join();
    // Let compaction catch up with the last of the churn, then measure.
    for (int round = 0; round < 50; ++round) {
        node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto stats = store.storage_stats();
    std::printf("[object soak] %.0f MiB appended in %.1f s (%.0f MiB/s); live %.1f MiB, peak disk "
                "%.1f MiB, peak RSS %.1f MiB, peak directory %.1f KiB; read p50 %.3f ms, p99 %.3f ms "
                "(%zu reads)\n",
                appended.load() / 1048576.0, seconds, appended.load() / 1048576.0 / seconds,
                stats.object_bytes / 1048576.0, peak_disk / 1048576.0, peak_rss / 1048576.0,
                peak_directory / 1024.0, percentile(reads, 0.5), percentile(reads, 0.99), reads.size());

    EXPECT_FALSE(failed.load());
    // The live set: kWriters × kPacksKept packs of 4 MiB, their index runs, the marker.
    EXPECT_LE(stats.object_bytes, kWriters * (kPacksKept + 1) * (kPackBytes + 8 * 1024) + 1024);
    // Disk and memory follow the live set (~100 MiB), not the bytes written.
    EXPECT_LT(peak_disk, 640ull * 1024 * 1024);
    EXPECT_LT(path_bytes(dir.path() / "objects"), 320ull * 1024 * 1024);
    EXPECT_LT(peak_directory, 4ull * 1024 * 1024);
    EXPECT_LT(peak_rss, starting_rss + 512ull * 1024 * 1024);
    EXPECT_LT(percentile(reads, 0.99), 10.0);
    // Nothing of it reached the replicated catalogue.
    EXPECT_LT(replication.shared_snapshot()->dump().size(), catalogue_before + 1024);
    node.stop();
}
