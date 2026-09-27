#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include "zetta/pubsub/event.hpp"

// Pub/sub event stores (#1026; decisions D1, D2, D5, D21, D23 in
// docs/plans/pubsub/decisions.md). See docs/pubsub/stores.md.
//
// Locks, not coroutines, by decision D1/D5: every queue owns its mutex and
// condition variable. Positions are 1-based and monotonic per store, so a
// reader's place never depends on which ring (or tier) an event lives in.
namespace zetta::pubsub {

using SteadyTime = std::chrono::steady_clock::time_point;

// The interface every store implements (D2): an append-only sequence readers
// address by position. Chosen at runtime, hence virtual.
class EventStore {
public:
    virtual ~EventStore() = default;

    // Append and return the event's position.
    virtual std::uint64_t append(Event event) = 0;
    // A copy of the event at `position`; nullopt when it was released or has
    // not been written yet.
    virtual std::optional<Event> read(std::uint64_t position) = 0;
    // The oldest position still held (== next_position() when empty).
    virtual std::uint64_t first_position() const = 0;
    // The position the next append receives.
    virtual std::uint64_t next_position() const = 0;
    // Drop everything below `position` (every entitled reader has passed it).
    virtual void release_before(std::uint64_t position) = 0;
    // Bytes held in memory / on disk.
    virtual std::size_t memory_bytes() const = 0;
    virtual std::size_t disk_bytes() const { return 0; }
    // Block until an event exists at `position`, the deadline passes, or
    // wake_all() is called. True when the event is available.
    virtual bool wait_for(std::uint64_t position, SteadyTime deadline) = 0;
    // Wake every waiter (shutdown, close, settings change).
    virtual void wake_all() = 0;
};

// One fixed-capacity ring of slots with its own mutex (D23). A ring is filled
// once, in order, and never wraps: when it is full the stack pushes a new one.
class RingBuffer {
public:
    RingBuffer(std::uint64_t first_position, std::size_t capacity);

    std::uint64_t first_position() const { return first_position_; }
    std::size_t capacity() const { return capacity_; }

    // Append at the next free slot; false when full.
    bool try_append(Event event);
    std::optional<Event> read(std::uint64_t position);
    bool full() const;
    std::size_t count() const;
    std::size_t memory_bytes() const;
    bool spilled() const;

    // Write the ring to `path` and drop its events from memory. False on I/O
    // failure (the ring then stays in memory). `written` receives the bytes
    // newly written (0 when the ring's file already exists from an earlier
    // spill and is simply reused) and `freed` the memory released.
    bool spill_to(const std::filesystem::path& path, std::size_t& written, std::size_t& freed);
    // Reload a spilled ring from its file (a read after spilling). False on I/O
    // failure or a corrupt file.
    bool load_back(std::size_t inline_threshold);
    // Every in-memory event, in order (the shutdown flush).
    std::vector<Event> snapshot() const;
    const std::filesystem::path& spill_path() const { return spill_path_; }
    // Reuse the ring for a new range (the free list).
    void reset(std::uint64_t first_position);

private:
    mutable std::mutex mutex_;
    std::uint64_t first_position_;
    std::size_t capacity_;
    std::vector<std::optional<Event>> slots_;
    std::size_t count_ = 0;
    std::size_t memory_bytes_ = 0;
    bool spilled_ = false;
    bool on_disk_ = false;  // the spill file exists (kept after a read-back)
    std::filesystem::path spill_path_;
};

struct RingStackOptions {
    std::size_t ring_capacity = 4096;   // slots per ring
    std::size_t free_list_limit = 2;    // spare rings kept for reuse
    std::size_t inline_threshold = kDefaultInlineThreshold;
};

// The default store (D21): a `std::deque` holding ring buffers. The stack's
// mutex protects changes to the stack — pushing a ring when the newest is full,
// removing rings every reader has passed — and lock order is always stack,
// then ring. The stack's condition variable wakes readers waiting for a new
// event.
class TopicRingStack final : public EventStore {
public:
    explicit TopicRingStack(RingStackOptions options = {});

    std::uint64_t append(Event event) override;
    std::optional<Event> read(std::uint64_t position) override;
    std::uint64_t first_position() const override;
    std::uint64_t next_position() const override;
    void release_before(std::uint64_t position) override;
    std::size_t memory_bytes() const override;
    std::size_t disk_bytes() const override;
    bool wait_for(std::uint64_t position, SteadyTime deadline) override;
    void wake_all() override;

    std::size_t ring_count() const;
    std::size_t spilled_ring_count() const;
    std::size_t free_ring_count() const;

    // Spill the oldest full, in-memory ring that is not the newest into
    // `directory`. Returns the memory freed, or 0 when nothing could spill.
    std::size_t spill_oldest(const std::filesystem::path& directory);

    // Every event still held in memory or spilled, in order (the shutdown
    // flush reads spilled rings back from disk).
    std::vector<Event> snapshot_all();

    // Restore events read back at start-up. Positions continue from
    // `next_position`, which must be past every restored event's position.
    void restore(std::vector<Event> events, std::uint64_t next_position);

private:
    std::shared_ptr<RingBuffer> ring_for(std::uint64_t position) const;  // stack lock held
    std::shared_ptr<RingBuffer> take_ring(std::uint64_t first_position);  // stack lock held

    RingStackOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable appended_;
    std::deque<std::shared_ptr<RingBuffer>> rings_;
    std::vector<std::shared_ptr<RingBuffer>> free_rings_;
    std::uint64_t released_before_ = 1;
    std::atomic<std::uint64_t> next_position_{1};
    std::size_t disk_bytes_ = 0;
    std::uint64_t wake_generation_ = 0;
};

// The alternative store (D2): a `std::queue` with its own mutex and condition
// variable. It only has a front, so it feeds one reader (a work queue): read()
// answers for first_position() only, and release_before() pops.
class QueueStore final : public EventStore {
public:
    std::uint64_t append(Event event) override;
    std::optional<Event> read(std::uint64_t position) override;
    std::uint64_t first_position() const override;
    std::uint64_t next_position() const override;
    void release_before(std::uint64_t position) override;
    std::size_t memory_bytes() const override;
    bool wait_for(std::uint64_t position, SteadyTime deadline) override;
    void wake_all() override;

private:
    mutable std::mutex mutex_;
    std::condition_variable appended_;
    std::queue<Event> queue_;
    std::uint64_t first_position_ = 1;
    std::uint64_t next_position_ = 1;
    std::size_t memory_bytes_ = 0;
    std::uint64_t wake_generation_ = 0;
};

}  // namespace zetta::pubsub
