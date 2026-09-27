// Pub/sub stores: RingBuffer, TopicRingStack, QueueStore (#1026).
#include <gtest/gtest.h>

#include <set>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/pubsub/store.hpp"

namespace zetta::pubsub {
namespace {

using namespace std::chrono_literals;

Event make_event(std::string text) {
    Event event;
    event.id = new_event_id();
    event.payload = Payload::make(std::move(text), kDefaultInlineThreshold);
    return event;
}

SteadyTime soon(std::chrono::milliseconds wait = 50ms) {
    return std::chrono::steady_clock::now() + wait;
}

TEST(PubSubRingBuffer, FillsInOrderAndRefusesWhenFull) {
    RingBuffer ring(10, 2);
    EXPECT_TRUE(ring.try_append(make_event("a")));
    EXPECT_TRUE(ring.try_append(make_event("b")));
    EXPECT_TRUE(ring.full());
    EXPECT_FALSE(ring.try_append(make_event("c")));
    EXPECT_EQ(ring.read(10)->payload.bytes(), "a");
    EXPECT_EQ(ring.read(11)->payload.bytes(), "b");
    EXPECT_FALSE(ring.read(12));
    EXPECT_FALSE(ring.read(9));
}

TEST(PubSubTopicRingStack, PushesARingWhenTheNewestIsFull) {
    TopicRingStack stack({.ring_capacity = 3});
    for (int index = 0; index < 7; ++index) stack.append(make_event(std::to_string(index)));
    EXPECT_EQ(stack.ring_count(), 3u);
    EXPECT_EQ(stack.first_position(), 1u);
    EXPECT_EQ(stack.next_position(), 8u);
    for (std::uint64_t position = 1; position <= 7; ++position) {
        EXPECT_EQ(stack.read(position)->payload.bytes(), std::to_string(position - 1));
    }
    EXPECT_FALSE(stack.read(8));
}

TEST(PubSubTopicRingStack, ReleasedRingsGoToTheFreeListAndAreReused) {
    TopicRingStack stack({.ring_capacity = 2, .free_list_limit = 1});
    for (int index = 0; index < 6; ++index) stack.append(make_event("x"));
    EXPECT_EQ(stack.ring_count(), 3u);
    stack.release_before(5);  // rings [1,2] and [3,4] are fully passed
    EXPECT_EQ(stack.ring_count(), 1u);
    EXPECT_EQ(stack.free_ring_count(), 1u);  // bounded by free_list_limit
    EXPECT_EQ(stack.first_position(), 5u);
    EXPECT_FALSE(stack.read(4));
    EXPECT_TRUE(stack.read(5));
    stack.append(make_event("y"));
    stack.append(make_event("z"));  // needs a new ring: taken from the free list
    EXPECT_EQ(stack.free_ring_count(), 0u);
    EXPECT_EQ(stack.read(8)->payload.bytes(), "z");
}

TEST(PubSubTopicRingStack, NewestRingIsKeptEvenWhenFullyReleased) {
    TopicRingStack stack({.ring_capacity = 2});
    stack.append(make_event("a"));
    stack.release_before(2);
    EXPECT_EQ(stack.ring_count(), 1u);
    EXPECT_EQ(stack.first_position(), 2u);
    EXPECT_EQ(stack.append(make_event("b")), 2u);
    EXPECT_EQ(stack.read(2)->payload.bytes(), "b");
}

TEST(PubSubTopicRingStack, ConcurrentProducersGetUniquePositions) {
    TopicRingStack stack({.ring_capacity = 64});
    constexpr int kProducers = 8;
    constexpr int kEach = 2000;
    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < kEach; ++index) {
                stack.append(make_event(std::to_string(producer) + ":" + std::to_string(index)));
            }
        });
    }
    for (auto& thread : producers) thread.join();
    EXPECT_EQ(stack.next_position(), static_cast<std::uint64_t>(kProducers * kEach + 1));
    std::set<std::string> seen;
    for (std::uint64_t position = 1; position < stack.next_position(); ++position) {
        auto event = stack.read(position);
        ASSERT_TRUE(event) << position;
        seen.insert(std::string(event->payload.bytes()));
    }
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kProducers * kEach));
}

TEST(PubSubTopicRingStack, WaitForWakesOnAppendAndTimesOut) {
    TopicRingStack stack;
    EXPECT_FALSE(stack.wait_for(1, soon(20ms)));
    std::thread producer([&] {
        std::this_thread::sleep_for(20ms);
        stack.append(make_event("late"));
    });
    EXPECT_TRUE(stack.wait_for(1, soon(5000ms)));
    producer.join();
}

TEST(PubSubTopicRingStack, WakeAllReleasesWaiters) {
    TopicRingStack stack;
    std::thread waker([&] {
        std::this_thread::sleep_for(20ms);
        stack.wake_all();
    });
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(stack.wait_for(1, soon(5000ms)));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 4000ms);
    waker.join();
}

TEST(PubSubTopicRingStack, SpillsOldRingsAndReadsThemBack) {
    zetta::test::TempDir directory;
    TopicRingStack stack({.ring_capacity = 2});
    for (int index = 0; index < 6; ++index) stack.append(make_event(std::string(300, 'a' + index)));
    const auto before = stack.memory_bytes();
    const auto freed = stack.spill_oldest(directory.path());
    EXPECT_GT(freed, 0u);
    EXPECT_EQ(stack.spilled_ring_count(), 1u);
    EXPECT_LT(stack.memory_bytes(), before);
    EXPECT_GT(stack.disk_bytes(), 0u);
    // The newest ring never spills.
    EXPECT_GT(stack.spill_oldest(directory.path()), 0u);
    EXPECT_EQ(stack.spill_oldest(directory.path()), 0u);
    // Reading a spilled position loads its ring back; the file is reused on
    // a second spill, so disk bytes do not double.
    const auto disk = stack.disk_bytes();
    EXPECT_EQ(stack.read(1)->payload.bytes(), std::string(300, 'a'));
    EXPECT_GT(stack.spill_oldest(directory.path()), 0u);
    EXPECT_EQ(stack.disk_bytes(), disk);
    // Releasing a spilled ring deletes its file.
    stack.release_before(3);
    EXPECT_LT(stack.disk_bytes(), disk);
}

TEST(PubSubTopicRingStack, SnapshotAndRestoreKeepEventsAndPositions) {
    zetta::test::TempDir directory;
    TopicRingStack stack({.ring_capacity = 2});
    for (int index = 0; index < 5; ++index) stack.append(make_event(std::to_string(index)));
    stack.release_before(2);
    stack.spill_oldest(directory.path());
    auto events = stack.snapshot_all();
    ASSERT_EQ(events.size(), 4u);  // positions 2..5
    EXPECT_EQ(events.front().payload.bytes(), "1");

    TopicRingStack restored({.ring_capacity = 2});
    restored.restore(std::move(events), stack.next_position());
    EXPECT_EQ(restored.first_position(), 2u);
    EXPECT_EQ(restored.next_position(), 6u);
    EXPECT_EQ(restored.read(2)->payload.bytes(), "1");
    EXPECT_EQ(restored.read(5)->payload.bytes(), "4");
    EXPECT_EQ(restored.append(make_event("5")), 6u);
}

TEST(PubSubQueueStore, FeedsOneReaderFromTheFront) {
    QueueStore queue;
    EXPECT_EQ(queue.append(make_event("a")), 1u);
    EXPECT_EQ(queue.append(make_event("b")), 2u);
    EXPECT_EQ(queue.read(1)->payload.bytes(), "a");
    EXPECT_FALSE(queue.read(2));  // only the front is readable
    queue.release_before(2);
    EXPECT_EQ(queue.first_position(), 2u);
    EXPECT_EQ(queue.read(2)->payload.bytes(), "b");
    EXPECT_GT(queue.memory_bytes(), 0u);
    queue.release_before(3);
    EXPECT_EQ(queue.memory_bytes(), 0u);
    EXPECT_FALSE(queue.wait_for(3, soon(10ms)));
}

}  // namespace
}  // namespace zetta::pubsub
