// The record-store soak (#1231): steady puts, conditional updates and retention
// purges on a running node, reporting what grows and what a read costs while the
// replicated catalogue is being saved. ZETTA_RECORD_SOAK_PUTS sets the number of
// puts (default 200,000; the review measurement used 1,000,000).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::data::RecordCondition;
using zetta::data::RecordStatus;
using zetta::data::RecordWrite;

std::uint64_t path_bytes(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return 0;
    if (std::filesystem::is_regular_file(path, error)) {
        return std::filesystem::file_size(path, error);
    }
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path, error)) {
        if (entry.is_regular_file(error)) total += entry.file_size(error);
    }
    return total;
}

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) return 0.0;
    std::ranges::sort(samples);
    return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1))];
}

// Recent keys a writer can update and a reader can read.
class RecentKeys {
public:
    void add(std::string collection, std::string key) {
        const std::lock_guard<std::mutex> lock(mutex_);
        keys_.emplace_back(std::move(collection), std::move(key));
        if (keys_.size() > 2048) keys_.pop_front();
    }
    std::optional<std::pair<std::string, std::string>> pick(std::mt19937_64& random) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (keys_.empty()) return std::nullopt;
        return keys_[random() % keys_.size()];
    }

private:
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> keys_;
};

}  // namespace

TEST(RecordStoreSoak, HistoryStaysBoundedAndReadsStayFastDuringSaves) {
    std::size_t total_puts = 200'000;
    if (const char* configured = std::getenv("ZETTA_RECORD_SOAK_PUTS")) {
        total_puts = std::strtoull(configured, nullptr, 10);
    }
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication};
    ASSERT_TRUE(node.start());
    zetta::data::RecordStore& store = *node.record_store();
    const std::vector<std::string> collections = {"agent_task_status", "ask_messages",
                                                  "webhook_events"};
    for (const std::string& name : collections) {
        zetta::data::RecordCollection collection;
        collection.name = name;
        collection.indexes = {"lookup"};
        collection.retention_ms = 3'000;
        ASSERT_TRUE(store.create_collection(collection).ok());
    }
    const std::string value(1'368, 'A');  // base64 of 1 KiB

    std::atomic<std::size_t> issued{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> updates{0};
    std::atomic<std::size_t> conflicts{0};
    std::atomic<std::size_t> failures{0};
    std::atomic<bool> writing{true};
    RecentKeys recent;

    std::vector<std::thread> writers;
    for (int writer = 0; writer < 4; ++writer) {
        writers.emplace_back([&, writer] {
            std::mt19937_64 random(static_cast<std::uint64_t>(writer) + 1);
            while (issued.fetch_add(1) < total_puts) {
                RecordWrite write;
                write.value = value;
                write.order = std::to_string(1'000'000'000'000LL + static_cast<long long>(completed.load()));
                const bool update = random() % 5 == 0;
                std::optional<std::pair<std::string, std::string>> target;
                if (update) target = recent.pick(random);
                if (target) {
                    const auto current = store.get(target->first, target->second);
                    if (current.ok()) {
                        write.collection = target->first;
                        write.key = target->second;
                        write.index = current.record->index;
                        write.condition = RecordCondition::IfRevision;
                        write.revision = current.record->revision;
                    }
                }
                if (write.key.empty()) {
                    write.collection = collections[random() % collections.size()];
                    write.key = zetta::util::new_uuid();
                    write.index = {{"lookup", "group-" + std::to_string(random() % 500)}};
                }
                const auto outcome = store.write(write);
                if (outcome.ok()) {
                    if (write.condition == RecordCondition::IfRevision) ++updates;
                    else recent.add(write.collection, write.key);
                } else if (outcome.status == RecordStatus::Conflict) {
                    ++conflicts;
                } else {
                    ++failures;
                }
                ++completed;
            }
        });
    }

    // A reader: get and list, each sample tagged with whether a catalogue save
    // was running when it started.
    std::mutex samples_mutex;
    std::vector<double> reads_during_save;
    std::vector<double> reads_otherwise;
    std::thread reader([&] {
        std::mt19937_64 random(99);
        while (writing.load()) {
            const auto target = recent.pick(random);
            if (!target) {
                std::this_thread::yield();
                continue;
            }
            const bool saving = node.persisting_replication();
            const auto started = std::chrono::steady_clock::now();
            store.get(target->first, target->second);
            zetta::data::RecordListRequest request;
            request.collection = target->first;
            request.index = "lookup";
            request.value = "group-" + std::to_string(random() % 500);
            request.limit = 50;
            store.list(request);
            const double elapsed = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
            const std::lock_guard<std::mutex> lock(samples_mutex);
            (saving || node.persisting_replication() ? reads_during_save : reads_otherwise)
                .push_back(elapsed);
        }
    });

    // This thread is the owner loop: it ticks (saves the catalogue at most once a
    // second when it changed, sweeps retention every second, compacts the record
    // log a bounded step per tick) and reports.
    std::vector<double> saves;
    std::uint64_t seen_saves = 0;
    std::uint64_t peak_rss = 0;
    const auto started = std::chrono::steady_clock::now();
    std::size_t next_report = 0;
    const auto report = [&](std::size_t done) {
        const auto memory = node.memory_snapshot();
        peak_rss = std::max(peak_rss, memory.allocator.rss_bytes);
        const auto usage = replication.memory_usage();
        std::vector<double> during;
        {
            const std::lock_guard<std::mutex> lock(samples_mutex);
            during = reads_during_save;
        }
        std::printf(
            "[record soak] %8zu puts %6.1f s | catalogue: memory %8.1f MiB, "
            "%7zu keys, disk %8.1f MiB | record log %8.1f MiB, %6llu records, %3llu segments, "
            "index memory %6.1f MiB | save p50 %7.1f ms "
            "p99 %7.1f ms (%zu saves) | read p99 during save %6.2f ms | RSS %8.1f MiB\n",
            done,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
            static_cast<double>(usage.bytes) / 1048576.0, replication.entry_count(),
            static_cast<double>(path_bytes(dir.path() / "replication.automerge")) / 1048576.0,
            static_cast<double>(path_bytes(dir.path() / "records")) / 1048576.0,
            static_cast<unsigned long long>(store.storage_stats().records),
            static_cast<unsigned long long>(store.storage_stats().segments),
            static_cast<double>(store.storage_stats().memory_bytes) / 1048576.0,
            percentile(saves, 0.5), percentile(saves, 0.99), saves.size(),
            percentile(during, 0.99), static_cast<double>(memory.allocator.rss_bytes) / 1048576.0);
        std::fflush(stdout);
    };
    // Records no longer touch the catalogue, so it would hardly ever save; keep
    // one catalogue key changing so a save runs every second and the reader
    // measures what a save costs it.
    auto last_catalogue_change = std::chrono::steady_clock::now();
    std::uint64_t catalogue_changes = 0;
    while (completed.load() < total_puts) {
        if (std::chrono::steady_clock::now() - last_catalogue_change >= std::chrono::milliseconds(500)) {
            last_catalogue_change = std::chrono::steady_clock::now();
            replication.set("soak_heartbeat:" + replication.node_id(),
                            nlohmann::json{{"beat", ++catalogue_changes}});
        }
        node.tick();
        const auto stats = node.replication_persist_stats();
        if (stats.saves != seen_saves) {
            seen_saves = stats.saves;
            saves.push_back(stats.last_ms);
        }
        if (completed.load() >= next_report) {
            report(completed.load());
            next_report += total_puts / 20;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto& writer : writers) writer.join();
    writing = false;
    reader.join();
    // Let the retention sweep catch up, then report the steady state.
    const auto settle = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - settle < std::chrono::seconds(12)) {
        node.tick();
        const auto stats = node.replication_persist_stats();
        if (stats.saves != seen_saves) {
            seen_saves = stats.saves;
            saves.push_back(stats.last_ms);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    report(completed.load());
    std::printf("[record soak] %zu updates, %zu conflicts, %zu failures; reads outside saves p99 "
                "%.3f ms (%zu samples), during saves p99 %.3f ms (%zu samples); peak RSS %.1f MiB\n",
                updates.load(), conflicts.load(), failures.load(),
                percentile(reads_otherwise, 0.99), reads_otherwise.size(),
                percentile(reads_during_save, 0.99), reads_during_save.size(),
                static_cast<double>(peak_rss) / 1048576.0);

    EXPECT_EQ(failures.load(), 0u);
    EXPECT_GT(updates.load(), total_puts / 10);
    // Bounded: once the retention window has passed, what is kept is the live
    // set plus what has not yet been compacted, not the history of every write.
    EXPECT_LT(path_bytes(dir.path() / "replication.automerge"), 4u * 1048576u);
    // Three seconds of writes are live at most; the rest is compacted away.
    EXPECT_LT(path_bytes(dir.path() / "records"), 160u * 1048576u);
    EXPECT_LT(store.storage_stats().records, 50'000u);
    EXPECT_LT(peak_rss, 1024ull * 1048576u);
    EXPECT_GT(saves.size(), 5u);
    EXPECT_LT(percentile(saves, 0.99), 50.0);
    EXPECT_LT(percentile(reads_during_save, 0.99), 10.0);
    EXPECT_LT(percentile(reads_otherwise, 0.99), 10.0);
    node.stop();
}
