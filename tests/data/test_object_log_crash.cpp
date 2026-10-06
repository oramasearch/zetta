// The object log's prefix guarantee under power loss and its fsync discipline
// (#1235, docs/data/object-store.md → "Durability"): whatever pages the disk
// had not made durable when the power went, what replays is the state after
// some prefix of the bucket's writes, never short of the last durable write or
// barrier; a barrier never answers before what it covers is fsynced; and a
// failed fsync fails the log until it is reopened from disk.
//
// A power loss is simulated on a copy of the log's files: the log reports each
// fsync through its sync observer, and the copy loses bytes of a segment past
// what an fsync made durable. The loss is cut off, zeroed in place (the size
// reached the disk, the data did not), or decided block by block at random (a
// middle block lost while a later one is kept, the file cut or not). The
// newest segment alone, each older segment alone, and every segment at once.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "../../src/core/data/object_log.hpp"
#include "temp_dir.hpp"

namespace {

using zetta::data::ObjectLog;
using zetta::test::TempDir;

constexpr std::uint64_t kSegmentBytes = 4096;  // a roll every few writes
constexpr std::int64_t kGraceMs = 24ll * 60 * 60 * 1000;
constexpr std::uint64_t kBlock = 512;  // the unit a simulated power loss keeps or drops

using State = std::map<std::string, std::string>;

// One write of the bucket's log, as a caller issues it.
struct Write {
    enum Kind { Put, Remove, Append, Upload } kind = Put;
    std::string key;
    std::string bytes;  // Put / Upload: the object; Append: the bytes appended
};

// What each fsync made durable: segment -> bytes. Called from any thread.
struct Synced {
    std::mutex mutex;
    std::map<std::uint32_t, std::uint64_t> bytes;
    void record(std::uint32_t segment, std::uint64_t durable) {
        const std::lock_guard lock(mutex);
        auto& held = bytes[segment];
        if (durable > held) held = durable;
    }
    void observe(ObjectLog& log) {
        log.set_sync_observer(
            [this](std::uint32_t segment, std::uint64_t durable) { record(segment, durable); });
    }
    std::uint64_t of(std::uint32_t segment) {
        const std::lock_guard lock(mutex);
        const auto found = bytes.find(segment);
        return found == bytes.end() ? 0 : found->second;
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

void step(State& state, const Write& write) {
    switch (write.kind) {
        case Write::Put:
        case Write::Upload: state[write.key] = write.bytes; break;
        case Write::Remove: state.erase(write.key); break;
        case Write::Append: state[write.key] += write.bytes; break;
    }
}

// The state after each prefix of `writes`: states[n] is after the first n.
std::vector<State> prefixes(const std::vector<Write>& writes) {
    std::vector<State> states(1);
    for (const Write& write : writes) {
        State next = states.back();
        step(next, write);
        states.push_back(std::move(next));
    }
    return states;
}

// Issue one write to the log; `state` is the state before it (for offsets).
void apply(ObjectLog& log, const Write& write, const State& state, bool durable,
           std::uint64_t& fresh) {
    ObjectLog::Written written;
    switch (write.kind) {
        case Write::Put: written = log.put(write.key, write.bytes, ++fresh, false, durable); break;
        case Write::Remove: written = log.remove(write.key, ++fresh, durable); break;
        case Write::Append: {
            const auto held = state.find(write.key);
            const std::uint64_t offset = held == state.end() ? 0 : held->second.size();
            written = log.append(write.key, offset, write.bytes, ++fresh, durable);
            break;
        }
        case Write::Upload: {
            // A staged put: the parts, then one commit frame.
            const std::uint64_t upload = ++fresh;
            constexpr std::size_t kPart = 700;
            for (std::size_t at = 0; at < write.bytes.size(); at += kPart) {
                const auto staged = log.stage(write.key, upload, at,
                                              std::string_view(write.bytes).substr(at, kPart), false);
                ASSERT_EQ(staged.result, ObjectLog::Result::Ok) << write.key;
            }
            written = log.commit(write.key, upload, write.bytes.size(), ++fresh, false, durable);
            break;
        }
    }
    ASSERT_EQ(written.result, ObjectLog::Result::Ok) << write.key;
    ASSERT_TRUE(written.durable);
}

// A mixed sequence: puts, overwrites, removes of live keys, appends, and
// staged puts of two to three parts.
std::vector<Write> mixed_writes(std::uint64_t seed, int count, const std::string& prefix = "k") {
    std::mt19937_64 random(seed);
    std::vector<Write> writes;
    State state;
    for (int index = 0; index < count; ++index) {
        Write write;
        write.key = prefix + std::to_string(random() % 17);
        const auto pick = random() % 10;
        if (pick < 2 && state.contains(write.key)) {
            write.kind = Write::Remove;
        } else if (pick < 5) {
            write.kind = Write::Append;
            write.bytes = value_of(index, 100 + random() % 300);
        } else if (pick < 7) {
            write.kind = Write::Upload;
            write.bytes = value_of(index, 1200 + random() % 900);
        } else {
            write.kind = Write::Put;
            write.bytes = value_of(index, 200 + random() % 400);
        }
        step(state, write);
        writes.push_back(std::move(write));
    }
    return writes;
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

// How a power loss treats the bytes no fsync covered: cut off (the size was
// not durable either), zero in place (it was: the file is extended over
// zeros), or each kBlock kept or zeroed at random with the size cut or kept.
struct Loss {
    enum Kind { Truncate, Zero, Random } kind = Truncate;
    std::uint64_t seed = 0;
    std::string name() const {
        return kind == Truncate ? "truncated"
               : kind == Zero   ? "zeroed"
                                : "random blocks, seed " + std::to_string(seed);
    }
};

void lose_unsynced(const std::filesystem::path& directory, std::uint32_t id, Synced& synced,
                   const Loss& loss) {
    const std::filesystem::path file = segment_file(directory, id);
    const std::uint64_t size = std::filesystem::file_size(file);
    const std::uint64_t durable = std::min(synced.of(id), size);
    if (durable == size) return;
    if (loss.kind == Loss::Truncate) {
        std::filesystem::resize_file(file, durable);
        return;
    }
    std::fstream out(file, std::ios::binary | std::ios::in | std::ios::out);
    const auto zero = [&](std::uint64_t from, std::uint64_t to) {
        out.seekp(static_cast<std::streamoff>(from));
        const std::string zeros(to - from, '\0');
        out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    };
    if (loss.kind == Loss::Zero) {
        zero(durable, size);
        return;
    }
    std::mt19937_64 random(loss.seed * 7919 + id);
    std::uint64_t kept_end = durable;
    for (std::uint64_t block = durable / kBlock * kBlock; block < size; block += kBlock) {
        const std::uint64_t from = std::max(block, durable);
        const std::uint64_t to = std::min(block + kBlock, size);
        if (random() % 2 == 0) {
            zero(from, to);
        } else {
            kept_end = to;
        }
    }
    out.close();
    if (random() % 2 == 0) std::filesystem::resize_file(file, kept_end);
}

// One simulated power loss over `live` (left untouched), checked against the
// writes: the recovered state is a prefix, at least `durable_through` long.
void expect_prefix_after_loss(const std::filesystem::path& live, Synced& synced,
                              const std::vector<std::uint32_t>& losing, const Loss& loss,
                              const std::vector<State>& states, std::size_t durable_through,
                              const std::string& scenario) {
    const TempDir crash;
    const std::filesystem::path copy = crash.path() / "bucket";
    std::filesystem::copy(live, copy, std::filesystem::copy_options::recursive);
    for (const std::uint32_t id : losing) lose_unsynced(copy, id, synced, loss);
    const State recovered = recover(copy);
    std::optional<std::size_t> matched;
    for (std::size_t n = states.size(); n-- > 0;) {
        if (states[n] == recovered) {
            matched = n;
            break;
        }
    }
    ASSERT_TRUE(matched.has_value())
        << scenario << " (" << loss.name() << "): the recovered state (" << recovered.size()
        << " keys) is not the state after any prefix of the writes";
    EXPECT_GE(*matched, durable_through)
        << scenario << " (" << loss.name()
        << "): a write acknowledged as durable (or before a barrier) was lost";
}

void expect_prefix_under_every_loss(const std::filesystem::path& live, Synced& synced,
                                    const std::vector<State>& states, std::size_t durable_through) {
    const std::vector<std::uint32_t> ids = segment_ids(live);
    ASSERT_GE(ids.size(), 2u);
    std::vector<Loss> losses{{Loss::Truncate, 0}, {Loss::Zero, 0}};
    for (std::uint64_t seed = 1; seed <= 6; ++seed) losses.push_back({Loss::Random, seed});
    for (const Loss& loss : losses) {
        expect_prefix_after_loss(live, synced, {ids.back()}, loss, states, durable_through,
                                 "the newest segment loses its unsynced pages");
        for (std::size_t older = 0; older + 1 < ids.size(); ++older) {
            expect_prefix_after_loss(live, synced, {ids[older]}, loss, states, durable_through,
                                     "segment " + std::to_string(ids[older]) +
                                         " alone loses its unsynced pages");
        }
        expect_prefix_after_loss(live, synced, ids, loss, states, durable_through,
                                 "every segment loses its unsynced pages");
    }
}

// Holds the first fsync of a segment file it is armed for until released, so
// a test can run a barrier while another path is inside its fsync.
struct Gate {
    std::atomic<bool> armed{false};
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    void install(ObjectLog& log) {
        log.set_fsync([this](const std::filesystem::path& file, int descriptor) {
            if (segment_id(file) && armed.exchange(false)) {
                entered.set_value();
                released.wait();
            }
            return ::fsync(descriptor);
        });
    }
};

// Runs a barrier while `inside` is held inside its fsync, and checks the
// barrier answered only after `segment` was fsynced through the bytes it held
// when the barrier began.
void expect_barrier_waits(ObjectLog& log, Gate& gate, Synced& synced,
                          const std::function<void()>& inside, const std::filesystem::path& live,
                          const std::function<std::uint32_t()>& covered_segment) {
    std::atomic<int> clock{0};
    std::atomic<int> covered_at{0};
    std::atomic<std::uint32_t> segment{0};
    std::atomic<std::uint64_t> covered{~0ull};
    log.set_sync_observer([&](std::uint32_t id, std::uint64_t durable) {
        synced.record(id, durable);
        if (id == segment.load() && durable >= covered.load() && covered_at == 0) covered_at = ++clock;
    });
    gate.armed = true;
    std::thread holder(inside);
    if (gate.entered.get_future().wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        gate.armed = false;
        holder.join();
        FAIL() << "the path under test never fsynced the segment";
    }
    segment = covered_segment();
    const std::uint64_t held = std::filesystem::file_size(segment_file(live, segment.load()));
    ASSERT_GT(held, synced.of(segment.load())) << "the segment should hold unsynced bytes";
    covered = held;
    std::atomic<int> returned_at{0};
    std::atomic<bool> barrier_ok{false};
    std::thread barrier([&] {
        barrier_ok = log.sync();
        returned_at = ++clock;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gate.release.set_value();
    barrier.join();
    holder.join();
    synced.observe(log);
    EXPECT_TRUE(barrier_ok);
    ASSERT_NE(covered_at.load(), 0) << "the covered bytes were never fsynced";
    EXPECT_LT(covered_at.load(), returned_at.load())
        << "the barrier answered before the bytes it covers were fsynced";
}

}  // namespace

// The issue's scenario: W1 lands in S1, the log rolls, W2 lands in S2, and the
// power fails before a barrier. Puts, overwrites, removes, appends and staged
// puts across many rolls, with durable writes and a barrier among them.
TEST(ObjectLogCrash, APowerLossKeepsAPrefixAcrossSegmentRolls) {
    const TempDir dir;
    const std::filesystem::path live = dir.path() / "bucket";
    ObjectLog log(live, kGraceMs, kSegmentBytes);
    Synced synced;
    synced.observe(log);
    std::string reason;
    ASSERT_TRUE(log.open(&reason)) << reason;

    const std::vector<Write> writes = mixed_writes(11, 120);
    const std::vector<State> states = prefixes(writes);
    std::size_t durable_through = 0;
    std::uint64_t fresh = 0;
    for (std::size_t index = 0; index < writes.size(); ++index) {
        const bool durable = index == 41 || index == 97;
        apply(log, writes[index], states[index], durable, fresh);
        if (durable) durable_through = index + 1;
        if (index == 70) {
            ASSERT_TRUE(log.sync());
            durable_through = index + 1;
        }
    }
    ASSERT_GE(segment_ids(live).size(), 10u) << "the writes should span many segment rolls";
    ASSERT_EQ(recover_copy(live), states.back());  // nothing lost: everything replays
    expect_prefix_under_every_loss(live, synced, states, durable_through);
}

// One writer and a thread running barriers beside it: every barrier that
// answered covers every write acknowledged before it began.
TEST(ObjectLogCrash, ConcurrentBarriersKeepWhatTheyCovered) {
    const TempDir dir;
    const std::filesystem::path live = dir.path() / "bucket";
    ObjectLog log(live, kGraceMs, kSegmentBytes);
    Synced synced;
    synced.observe(log);
    std::string reason;
    ASSERT_TRUE(log.open(&reason)) << reason;

    std::vector<Write> writes = mixed_writes(23, 150);
    // The last few come after the last barrier, so a loss has bytes to drop.
    for (int index = 0; index < 3; ++index) {
        writes.push_back(Write{Write::Put, "tail" + std::to_string(index), value_of(900 + index, 300)});
    }
    const std::vector<State> states = prefixes(writes);
    const std::size_t barriered = writes.size() - 3;
    std::atomic<std::size_t> acknowledged{0};
    std::atomic<std::size_t> durable_through{0};
    std::atomic<bool> done{false};
    std::atomic<int> barriers{0};
    std::thread barrier([&] {
        while (!done) {
            const std::size_t before = acknowledged.load();
            if (log.sync()) {
                std::size_t held = durable_through.load();
                while (before > held && !durable_through.compare_exchange_weak(held, before)) {
                }
                ++barriers;
            }
        }
    });
    std::uint64_t fresh = 0;
    for (std::size_t index = 0; index < barriered; ++index) {
        apply(log, writes[index], states[index], index % 37 == 36, fresh);
        acknowledged = index + 1;
    }
    done = true;
    barrier.join();
    ASSERT_GT(barriers.load(), 0);
    for (std::size_t index = barriered; index < writes.size(); ++index) {
        apply(log, writes[index], states[index], false, fresh);
    }
    ASSERT_EQ(recover_copy(live), states.back());
    expect_prefix_under_every_loss(live, synced, states, durable_through.load());
}

// A process crash leaves bytes in the page cache that no fsync covered (in the
// last two segments, for a log an older build wrote); the node restarts, keeps
// writing across rolls, and then the power fails.
TEST(ObjectLogCrash, APowerLossAfterAProcessCrashAndRestartKeepsAPrefix) {
    const TempDir dir;
    const TempDir restarted;
    const std::filesystem::path first = dir.path() / "bucket";
    const std::filesystem::path live = restarted.path() / "bucket";
    std::vector<Write> writes = mixed_writes(31, 30, "a");
    // Other keys afterwards, so the new writes cannot hide a loss of the old ones.
    const std::vector<Write> more = mixed_writes(37, 50, "b");
    writes.insert(writes.end(), more.begin(), more.end());
    const std::vector<State> states = prefixes(writes);
    std::uint64_t fresh = 0;
    Synced synced;
    {
        ObjectLog log(first, kGraceMs, kSegmentBytes);
        synced.observe(log);
        std::string reason;
        ASSERT_TRUE(log.open(&reason)) << reason;
        for (std::size_t index = 0; index < 30; ++index) {
            apply(log, writes[index], states[index], false, fresh);
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
    for (std::size_t index = 30; index < writes.size(); ++index) {
        apply(log, writes[index], states[index], false, fresh);
    }
    ASSERT_EQ(recover_copy(live), states.back());
    expect_prefix_under_every_loss(live, synced, states, 0);
}

// A log a build from before #1235 wrote can hold unsynced bytes in its last
// two segments. Startup ends the log at the first torn frame in them: a tear
// in the second-to-last drops the whole last segment.
TEST(ObjectLogCrash, ALogFromBeforeTheFixEndsAtItsFirstTornFrame) {
    const TempDir dir;
    const std::filesystem::path live = dir.path() / "bucket";
    const std::vector<Write> writes = mixed_writes(41, 60);
    const std::vector<State> states = prefixes(writes);
    Synced synced;
    {
        ObjectLog log(live, kGraceMs, kSegmentBytes);
        synced.observe(log);
        std::string reason;
        ASSERT_TRUE(log.open(&reason)) << reason;
        std::uint64_t fresh = 0;
        for (std::size_t index = 0; index < writes.size(); ++index) {
            apply(log, writes[index], states[index], false, fresh);
        }
        log.set_sync_observer(nullptr);
    }
    const std::vector<std::uint32_t> ids = segment_ids(live);
    ASSERT_GE(ids.size(), 3u);
    // The old build never fsynced the last two; only the second-to-last loses
    // its pages, so the newest is whole and would replay past the tear.
    synced.bytes.erase(ids[ids.size() - 2]);
    std::vector<Loss> losses{{Loss::Zero, 0}};
    for (std::uint64_t seed = 1; seed <= 6; ++seed) losses.push_back({Loss::Random, seed});
    for (const Loss& loss : losses) {
        expect_prefix_after_loss(live, synced, {ids[ids.size() - 2]}, loss, states, 0,
                                 "the second-to-last segment of an old log loses its pages");
    }
}

// Compaction deletes a segment once newer frames replaced all of it. If those
// frames were still unsynced, a power loss would keep neither: the keys would
// vanish while later writes stayed. The removal itself is made durable by an
// fsync of the directory after the unlink.
TEST(ObjectLogCrash, ADeadSegmentIsDeletedOnlyAfterWhatReplacedItIsDurable) {
    const TempDir dir;
    const std::filesystem::path live = dir.path() / "bucket";
    ObjectLog log(live, kGraceMs, kSegmentBytes);
    Synced synced;
    synced.observe(log);
    std::vector<bool> directory_syncs;  // per directory fsync: was the first segment gone?
    std::mutex syncs_mutex;
    log.set_fsync([&](const std::filesystem::path& file, int descriptor) {
        if (!segment_id(file) && file == live) {
            const std::lock_guard lock(syncs_mutex);
            directory_syncs.push_back(!std::filesystem::exists(segment_file(live, 1)));
        }
        return ::fsync(descriptor);
    });
    std::string reason;
    ASSERT_TRUE(log.open(&reason)) << reason;

    std::vector<Write> writes;
    std::uint64_t fresh = 0;
    const auto write_all = [&](const std::string& prefix, int round) {
        for (int index = 0; index < 7; ++index) {
            const Write write{Write::Put, prefix + std::to_string(index), value_of(round * 100 + index, 600)};
            apply(log, write, {}, false, fresh);
            writes.push_back(write);
        }
    };
    write_all("x", 0);  // the first segment: every frame will be replaced
    write_all("y", 1);  // the second: stays live
    write_all("x", 2);  // replaces the first segment, in the unsynced newest one
    const std::vector<std::uint32_t> before = segment_ids(live);
    ASSERT_GE(before.size(), 3u);
    ASSERT_EQ(before.front(), 1u);
    for (int step = 0; step < 4; ++step) log.maintain(now_ms());
    const std::vector<std::uint32_t> after = segment_ids(live);
    ASSERT_LT(after.size(), before.size()) << "compaction should have deleted the dead first segment";
    ASSERT_EQ(after.front(), before[1]);
    {
        const std::lock_guard lock(syncs_mutex);
        EXPECT_TRUE(std::ranges::find(directory_syncs, true) != directory_syncs.end())
            << "the directory was not fsynced after the dead segment was unlinked";
    }

    const std::vector<State> states = prefixes(writes);
    ASSERT_EQ(recover_copy(live), states.back());
    expect_prefix_under_every_loss(live, synced, states, 0);
}

// open() fsyncs the parent of every directory it creates, so the bucket's
// directory itself survives a power loss.
TEST(ObjectLogCrash, OpenFsyncsTheDirectoriesItCreates) {
    const TempDir dir;
    const std::filesystem::path bucket = dir.path() / "objects" / "bucket";
    ObjectLog log(bucket, kGraceMs, kSegmentBytes);
    std::vector<std::filesystem::path> synced;
    log.set_fsync([&](const std::filesystem::path& file, int descriptor) {
        synced.push_back(file);
        return ::fsync(descriptor);
    });
    std::string reason;
    ASSERT_TRUE(log.open(&reason)) << reason;
    EXPECT_TRUE(std::ranges::find(synced, dir.path()) != synced.end()) << "the parent of objects/";
    EXPECT_TRUE(std::ranges::find(synced, dir.path() / "objects") != synced.end())
        << "the parent of the bucket's directory";
}

// A barrier never answers before the bytes it covers are fsynced, even while
// another path is inside its own fsync of the same segment: a durable write,
// compaction's fsync before it deletes a dead segment, and a roll.
TEST(ObjectLogCrash, ABarrierWaitsForAnFsyncAlreadyRunning) {
    const auto newest = [](const std::filesystem::path& live) { return segment_ids(live).back(); };
    std::uint64_t fresh = 0;
    {
        SCOPED_TRACE("a durable write");
        const TempDir dir;
        const std::filesystem::path live = dir.path() / "bucket";
        ObjectLog log(live, kGraceMs, kSegmentBytes);
        Synced synced;
        Gate gate;
        synced.observe(log);
        gate.install(log);
        std::string reason;
        ASSERT_TRUE(log.open(&reason)) << reason;
        ASSERT_EQ(log.put("a", value_of(1, 300), ++fresh, false, false).result, ObjectLog::Result::Ok);
        expect_barrier_waits(
            log, gate, synced,
            [&] { EXPECT_TRUE(log.put("b", value_of(2, 300), ++fresh, false, true).durable); }, live,
            [&] { return newest(live); });
    }
    {
        SCOPED_TRACE("compaction deleting a dead segment");
        const TempDir dir;
        const std::filesystem::path live = dir.path() / "bucket";
        ObjectLog log(live, kGraceMs, kSegmentBytes);
        Synced synced;
        Gate gate;
        synced.observe(log);
        gate.install(log);
        std::string reason;
        ASSERT_TRUE(log.open(&reason)) << reason;
        for (const std::string prefix : {"x", "y", "x"}) {
            for (int index = 0; index < 7; ++index) {
                ASSERT_EQ(log.put(prefix + std::to_string(index), value_of(index, 600), ++fresh, false, false).result,
                          ObjectLog::Result::Ok);
            }
        }
        expect_barrier_waits(log, gate, synced, [&] { log.maintain(now_ms()); }, live,
                             [&] { return newest(live); });
        EXPECT_EQ(segment_ids(live).size(), 2u) << "compaction should have deleted the first segment";
    }
    {
        SCOPED_TRACE("a roll");
        const TempDir dir;
        const std::filesystem::path live = dir.path() / "bucket";
        ObjectLog log(live, kGraceMs, kSegmentBytes);
        Synced synced;
        Gate gate;
        synced.observe(log);
        gate.install(log);
        std::string reason;
        ASSERT_TRUE(log.open(&reason)) << reason;
        int index = 0;
        while (std::filesystem::file_size(segment_file(live, 1)) < kSegmentBytes) {
            ASSERT_EQ(log.put("r" + std::to_string(index), value_of(index, 500), ++fresh, false, false).result,
                      ObjectLog::Result::Ok);
            ++index;
        }
        expect_barrier_waits(
            log, gate, synced,
            [&] {
                EXPECT_EQ(log.put("rolled", value_of(99, 500), ++fresh, false, false).result,
                          ObjectLog::Result::Ok);
            },
            live, [] { return std::uint32_t{1}; });
        EXPECT_EQ(segment_ids(live).size(), 2u);
    }
}

// A failed fsync fails the log: the kernel may have dropped the dirty pages
// and marked them clean, so a retry could succeed without them. Writes and
// barriers are refused until maintain() reopens the log from disk; then it
// works again. Through a durable write, a barrier and a roll.
TEST(ObjectLogCrash, AFailedFsyncFailsTheLogUntilItIsReopened) {
    for (const std::string how : {"durable write", "barrier", "roll"}) {
        SCOPED_TRACE(how);
        const TempDir dir;
        const std::filesystem::path live = dir.path() / "bucket";
        ObjectLog log(live, kGraceMs, kSegmentBytes);
        std::atomic<bool> fault{false};
        log.set_fsync([&](const std::filesystem::path&, int descriptor) {
            if (fault.exchange(false)) {
                errno = EIO;
                return -1;
            }
            return ::fsync(descriptor);
        });
        std::string reason;
        ASSERT_TRUE(log.open(&reason)) << reason;
        std::uint64_t fresh = 0;
        ASSERT_EQ(log.put("before", "kept", ++fresh, false, false).result, ObjectLog::Result::Ok);
        fault = true;
        if (how == "durable write") {
            const auto written = log.put("durable", "maybe lost", ++fresh, false, true);
            EXPECT_FALSE(written.durable) << "a durable write whose fsync failed must not be acknowledged";
        } else if (how == "barrier") {
            EXPECT_FALSE(log.sync());
        } else {
            ObjectLog::Result result = ObjectLog::Result::Ok;
            for (int index = 0; index < 40 && result == ObjectLog::Result::Ok; ++index) {
                result = log.put("r" + std::to_string(index), value_of(index, 500), ++fresh, false, false).result;
            }
            EXPECT_EQ(result, ObjectLog::Result::Io) << "the write after a failed roll must fail";
        }
        ASSERT_TRUE(log.failed());
        // A later fsync would succeed, but nothing is acknowledged meanwhile.
        EXPECT_EQ(log.put("after", "refused", ++fresh, false, false).result, ObjectLog::Result::Io);
        EXPECT_EQ(log.append("before", 4, "more", ++fresh, false).result, ObjectLog::Result::Io);
        EXPECT_FALSE(log.sync());
        EXPECT_FALSE(log.stat("after").has_value());
        // Maintenance reopens it from disk, as a restart would.
        log.maintain(now_ms());
        ASSERT_FALSE(log.failed());
        ASSERT_TRUE(log.stat("before").has_value());
        EXPECT_EQ(log.put("after", "accepted", ++fresh, false, true).result, ObjectLog::Result::Ok);
        EXPECT_TRUE(log.sync());
        EXPECT_EQ(recover_copy(live).at("after"), "accepted");
    }
}
