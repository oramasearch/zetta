# Dispatch

**Issue:** [#1026](https://github.com/oramasearch/zetta/issues/1026) · **Decision:** D24 ·
**Code:** `include/zetta/pubsub/dispatcher.hpp`, `src/core/pubsub/dispatcher.cpp` ·
**Tests:** `tests/pubsub/test_dispatch_and_resources.cpp`

The `Dispatcher` runs pub/sub work that should not run on the publisher's thread: inbox
drains, and appends for topics with `producer_queue`. Work is submitted under a key (the
target address); work for one key always runs in submission order.

| Mode | What runs the work |
|---|---|
| `PerKey` | a thread per key (the default while keys are few) |
| `Pool` | a shared pool; each key sticks to one worker, workers take their keys round robin, at most `per_turn_cap` jobs per turn |
| `Inline` | the submitting thread |
| `Auto` | `PerKey`, moving to `Pool` when keys exceed the threshold and back at half of it |

The threshold and pool size come from the detected CPUs (2 × CPUs and CPUs,
[limits.md](limits.md)). A topic can be pinned to `PerKey` or `Inline` through
`TopicSettings::dispatch`. Every switch is logged:
`pubsub: INFO dispatch: 34 topics > threshold 32; moving to a pool of 16 threads`.
