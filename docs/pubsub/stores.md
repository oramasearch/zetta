# Stores: TopicRingStack, RingBuffer, QueueStore

**Issue:** [#1026](https://github.com/oramasearch/zetta/issues/1026) · **Decisions:**
D1, D2, D5, D21, D23 · **Code:** `include/zetta/pubsub/store.hpp`,
`src/core/pubsub/store.cpp` · **Tests:** `tests/pubsub/test_stores.cpp`

Every store implements the `EventStore` interface: an append-only sequence that
readers address by **position** (1-based, monotonic). A position never depends on
where the event lives (which ring, memory or disk), which is what lets readers
cross rings and tiers without caring.

## TopicRingStack (the default)

A `std::deque` that **holds ring buffers** (D21). Each `RingBuffer` has a fixed
number of slots and its own mutex; it fills once, in order, and never wraps.

- **Append:** under the stack's mutex, take the next position; when the newest ring
  is full, push a new ring (from the free list when one is spare), then append into
  it under the ring's mutex. **Lock order: stack, then ring.** Waiting readers are
  woken through the stack's condition variable.
- **Read:** find the ring under the stack's mutex, then read under the ring's mutex
  only, so readers of old rings do not hold up publishers.
- **Release:** `release_before(position)` removes whole rings every reader has
  passed (the newest ring always stays) and keeps up to `free_list_limit` of them
  for reuse.
- **Spill:** `spill_oldest(directory)` writes the oldest full ring that is not the
  newest to `ring-<first position>.frames` (the frame format of
  [events.md](events.md)) and drops it from memory; reading a spilled position
  loads the ring back, and a later spill reuses the existing file. Releasing a
  spilled ring deletes its file.
- **Flush and restore:** `snapshot_all()` returns every held event, spilled ones
  included; `restore(events, next_position)` rebuilds the stack at start-up with
  positions continuing where they stopped.

## QueueStore (the alternative)

A `std::queue` with its own mutex and condition variable. It only has a front, so
`read` answers for `first_position()` alone and `release_before` pops: it feeds one
reader and suits work-queue topics, not broadcast.

## Waiting

`wait_for(position, deadline)` blocks until an event exists at `position`, the
deadline passes, or `wake_all()` is called (shutdown, close). No coroutines in
stage 1 (D5): the waiter is a thread.
