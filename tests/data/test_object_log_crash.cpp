// The object log's prefix guarantee under power loss (#1235,
// docs/data/object-store.md → "Durability"): whatever pages the disk had not
// made durable when the power went, what replays is the state after some
// prefix of the bucket's writes, never short of the last durable write or
// barrier.
//
// A power loss is simulated on a copy of the log's files: the log reports each
// fsync through its sync observer, and the copy loses (truncated, or zeroed in
// place) the bytes of a segment past what an fsync made durable. The newest
// segment alone, each older segment alone, and every segment at once.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../../src/core/data/object_log.hpp"
#include "temp_dir.hpp"

namespace {

using zetta::data::ObjectLog;
using zetta::test::TempDir;

constexpr std::uint64_t kSegmentBytes = 4096;  // a roll every few writes
constexpr std::int64_t kGraceMs = 24ll * 60 * 60 * 1000;

using State = std::map<std::string, std::string>;

struct Write {
    std::string key;
    std::string value;  // empty: a remove
};

// What each fsync made durable: segment -> bytes.
struct Synced {
    std::map<std::uint32_t, std::uint64_t> bytes;
    void observe(ObjectLog& log) {
        log.set_sync_observer([this](std::uint32_t segment, std::uint64_t durable) {
            auto& held = bytes[segment];
            if (durable > held) held = durable;
        });
    }
};

std::string value_of(int index, std::size_t size) {
    std::string bytes(size, '\0');
    for (std::size_t at = 0; at < size; ++at) bytes[at] = static_cast<char>('a' + (index * 7 + at) % 26);
    return "w" + std::to_string(index) + ":" + bytes;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Apply one write to the log (put or remove), as lake's store would.
void apply(ObjectLog& log, const Write& write, bool durable, std::uint64_t& fresh) {
    if (write.value.empty()) {
        const auto written = log.remove(write.key, ++fresh, durable);
        ASSERT_EQ(written.result, ObjectLog::Result::Ok) << write.key;
        ASSERT_TRUE(written.durable);
        return;
    }
    const auto written = log.put(write.key, write.value, ++fresh, false, durable);
    ASSERT_EQ(written.result, ObjectLog::Result::Ok) << write.key;
    ASSERT_TRUE(written.durable);
}

// The state after each prefix of `writes`: states[n] is after the first n.
std::vector<State> prefixes(const std::vector<Write>& writes) {
    std::vector<State> states(1);
    for (const Write& write : writes) {
        State next = states.back();
        if (write.value.empty()) {
            next.erase(write.key);
        } else {
            next[write.key] = write.value;
        }
        states.push_back(std::move(next));
    }
    return states;
}

State recover(const std::filesystem::path& directory) {
    ObjectLog log(directory, kGraceMs, kSegmentBytes);
    std::string reason;
    EXPECT_TRUE(log.open(&reason)) << reason;
    State state;
    std::vector<std::string> keys;
    log.each([&](const zetta::data::ObjectVersion& version) {
        if (!version.deleted) keys.push_back(version.key);
    });
    for (const std::string& key : keys) {
        ObjectLog::ReadSnapshot snapshot;
        EXPECT_EQ(log.snapshot(key, 0, std::nullopt, snapshot), ObjectLog::ReadStatus::Ok) << key;
        std::string bytes;
        EXPECT_TRUE(snapshot.read_into(0, snapshot.length, bytes)) << key;
        state[key] = bytes;
    }
    return state;
}

// Recover from a copy, so the live log's directory is left as it is.
State recover_copy(const std::filesystem::path& live) {
    const TempDir copy;
    std::filesystem::copy(live, copy.path() / "bucket", std::filesystem::copy_options::recursive);
    return recover(copy.path() / "bucket");
}

std::optional<std::uint32_t> segment_id(const std::filesystem::path& file) {
    const std::string name = file.filename().string();
    unsigned id = 0;
    if (std::sscanf(name.c_str(), "segment-%08u.log", &id) != 1) return std::nullopt;
    return id;
}

std::vector<std::uint32_t> segment_ids(const std::filesystem::path& directory) {
    std::vector<std::uint32_t> ids;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (const auto id = segment_id(entry.path())) ids.push_back(*id);
    }
    std::ranges::sort(ids);
    return ids;
}

std::filesystem::path segment_file(const std::filesystem::path& directory, std::uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "segment-%08u.log", id);
    return directory / name;
}

enum class Fill { Truncate, Zero };

// The power loss: the bytes of segment `id` past what was fsynced are gone,
// cut off (the size was not durable either) or zero in place (it was).
void lose_unsynced(const std::filesystem::path& directory, std::uint32_t id, const Synced& synced,
                   Fill fill) {
    const std::filesystem::path file = segment_file(directory, id);
    const std::uint64_t size = std::filesystem::file_size(file);
    const auto held = synced.bytes.find(id);
    const std::uint64_t durable = held == synced.bytes.end() ? 0 : std::min(held->second, size);
    if (durable == size) return;
    if (fill == Fill::Truncate) {
        std::filesystem::resize_file(file, durable);
        return;
    }
    std::fstream out(file, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(static_cast<std::streamoff>(durable));
    const std::string zeros(size - durable, '\0');
    out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
}

// One simulated power loss over `live` (left untouched), checked against the
// writes: the recovered state is a prefix, at least `durable_through` long.
void expect_prefix_after_loss(const std::filesystem::path& live, const Synced& synced,
                              const std::vector<std::uint32_t>& losing, Fill fill,
                              const std::vector<State>& states, std::size_t durable_through,
                              const std::string& scenario) {
    const TempDir crash;
    const std::filesystem::path copy = crash.path() / "bucket";
    std::filesystem::copy(live, copy, std::filesystem::copy_options::recursive);
    for (const std::uint32_t id : losing) lose_unsynced(copy, id, synced, fill);
    const State recovered = recover(copy);
    std::optional<std::size_t> matched;
    for (std::size_t n = states.size(); n-- > 0;) {
        if (states[n] == recovered) {
            matched = n;
            break;
        }
    }
    ASSERT_TRUE(matched.has_value())
        << scenario << ": the recovered state (" << recovered.size()
        << " keys) is not the state after any prefix of the writes";
    EXPECT_GE(*matched, durable_through)
        << scenario << ": a write acknowledged as durable (or before a barrier) was lost";
}

void expect_prefix_under_every_loss(const std::filesystem::path& live, const Synced& synced,
                                    const std::vector<State>& states, std::size_t durable_through) {
    const std::vector<std::uint32_t> ids = segment_ids(live);
    ASSERT_GE(ids.size(), 2u);
    for (const Fill fill : {Fill::Truncate, Fill::Zero}) {
        const std::string how = fill == Fill::Truncate ? " (truncated)" : " (zeroed)";
        expect_prefix_after_loss(live, synced, {ids.back()}, fill, states, durable_through,
                                 "the newest segment loses its unsynced pages" + how);
        for (std::size_t older = 0; older + 1 < ids.size(); ++older) {
            expect_prefix_after_loss(live, synced, {ids[older]}, fill, states, durable_through,
                                     "segment " + std::to_string(ids[older]) +
                                         " alone loses its unsynced pages" + how);
        }
        expect_prefix_after_loss(live, synced, ids, fill, states, durable_through,
                                 "every segment loses its unsynced pages" + how);
    }
}

}  // namespace

// The issue's scenario: W1 lands in S1, the log rolls, W2 lands in S2, and the
// power fails before a barrier. Puts, overwrites and removes across many rolls,
// with two durable writes and a barrier among them.
TEST(ObjectLogCrash, APowerLossKeepsAPrefixAcrossSegmentRolls) {
    const TempDir dir;
    const std::filesystem::path live = dir.path() / "bucket";
    ObjectLog log(live, kGraceMs, kSegmentBytes);
    Synced synced;
    synced.observe(log);
    std::string reason;
    ASSERT_TRUE(log.open(&reason)) << reason;

    std::vector<Write> writes;
    std::size_t durable_through = 0;
    std::uint64_t fresh = 0;
    for (int index = 0; index < 120; ++index) {
        Write write{"k" + std::to_string(index % 23), ""};
        // Every eleventh write removes its key when it is live.
        if (index % 11 != 10 || !prefixes(writes).back().contains(write.key)) {
            write.value = value_of(index, 200 + (index * 37) % 400);
        }
        const bool durable = index == 41 || index == 97;
        apply(log, write, durable, fresh);
        writes.push_back(write);
        if (durable) durable_through = writes.size();
        if (index == 70) {
            ASSERT_TRUE(log.sync());
            durable_through = writes.size();
        }
    }
    ASSERT_GE(segment_ids(live).size(), 10u) << "the writes should span many segment rolls";
    const std::vector<State> states = prefixes(writes);
    ASSERT_EQ(recover_copy(live), states.back());  // nothing lost: everything replays
    expect_prefix_under_every_loss(live, synced, states, durable_through);
}

// A process crash leaves bytes in the page cache that no fsync covered (in the
// last two segments, for a log an older build wrote); the node restarts, keeps
// writing across rolls, and then the power fails.
TEST(ObjectLogCrash, APowerLossAfterAProcessCrashAndRestartKeepsAPrefix) {
    const TempDir dir;
    const TempDir restarted;
    const std::filesystem::path first = dir.path() / "bucket";
    const std::filesystem::path live = restarted.path() / "bucket";
    std::vector<Write> writes;
    std::uint64_t fresh = 0;
    Synced synced;
    {
        ObjectLog log(first, kGraceMs, kSegmentBytes);
        synced.observe(log);
        std::string reason;
        ASSERT_TRUE(log.open(&reason)) << reason;
        for (int index = 0; index < 30; ++index) {
            Write write{"a" + std::to_string(index % 9), value_of(index, 300)};
            apply(log, write, false, fresh);
            writes.push_back(write);
        }
        // The process dies: its files, page cache included, are what the
        // restarted process opens (copied before the destructor fsyncs anything).
        std::filesystem::copy(first, live, std::filesystem::copy_options::recursive);
        log.set_sync_observer(nullptr);
        // As a build from before #1235 left it: the segment before the newest
        // was never fsynced (its roll did not, and maintenance had not yet).
        const std::vector<std::uint32_t> ids = segment_ids(live);
        ASSERT_GE(ids.size(), 2u);
        synced.bytes.erase(ids[ids.size() - 2]);
    }
    ObjectLog log(live, kGraceMs, kSegmentBytes);
    synced.observe(log);
    std::string reason;
    ASSERT_TRUE(log.open(&reason)) << reason;
    for (int index = 30; index < 80; ++index) {
        // Other keys, so the new writes cannot hide a loss of the old ones.
        Write write{"b" + std::to_string(index % 9), value_of(index, 300)};
        apply(log, write, false, fresh);
        writes.push_back(write);
    }
    const std::vector<State> states = prefixes(writes);
    ASSERT_EQ(recover_copy(live), states.back());
    expect_prefix_under_every_loss(live, synced, states, 0);
}

// Compaction deletes a segment once newer frames replaced all of it. If those
// frames were still unsynced, a power loss would keep neither: the keys would
// vanish while later writes stayed.
TEST(ObjectLogCrash, ADeadSegmentIsDeletedOnlyAfterWhatReplacedItIsDurable) {
    const TempDir dir;
    const std::filesystem::path live = dir.path() / "bucket";
    ObjectLog log(live, kGraceMs, kSegmentBytes);
    Synced synced;
    synced.observe(log);
    std::string reason;
    ASSERT_TRUE(log.open(&reason)) << reason;

    std::vector<Write> writes;
    std::uint64_t fresh = 0;
    const auto write_all = [&](const std::string& prefix, int round) {
        for (int index = 0; index < 7; ++index) {
            Write write{prefix + std::to_string(index), value_of(round * 100 + index, 600)};
            apply(log, write, false, fresh);
            writes.push_back(write);
        }
    };
    write_all("x", 0);  // the first segment: every frame will be replaced
    write_all("y", 1);  // the second: stays live
    write_all("x", 2);  // replaces the first segment, in the unsynced newest one
    const std::vector<std::uint32_t> before = segment_ids(live);
    ASSERT_GE(before.size(), 3u);
    for (int step = 0; step < 4; ++step) log.maintain(now_ms());
    const std::vector<std::uint32_t> after = segment_ids(live);
    ASSERT_LT(after.size(), before.size()) << "compaction should have deleted the dead first segment";
    ASSERT_EQ(after.front(), before[1]);

    const std::vector<State> states = prefixes(writes);
    ASSERT_EQ(recover_copy(live), states.back());
    expect_prefix_under_every_loss(live, synced, states, 0);
}
