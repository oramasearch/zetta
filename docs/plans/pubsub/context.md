# What pub/sub builds on

Read from the pinned checkout (zetta `2aa1b8a`, Capy `ce1c7d7`) on 2026-09-27.

## The runtime we have

`include/zetta/runtime.hpp` re-exports Boost.Capy (ours, vendored at
`modules/capy`):

| Primitive | What it gives us | Thread safety (from Capy's own docs) |
|---|---|---|
| `task<T>` / `io_task<T>` | lazy coroutine | — |
| `spawn`, `when_all`, `when_any`, `timeout` | composition | — |
| `thread_pool` | worker threads + an executor | safe |
| `strand` | serializes work posted to it | **shared objects: safe** |
| `async_event` | broadcast wake of N awaiting coroutines; zero allocation (intrusive waiter list); cancellation via `stop_token` | **shared objects: unsafe — one executor only**; only the stop callback may fire from another thread |
| `async_mutex` | coroutine mutex | **one executor only**, same as `async_event` |

Two things follow for pub/sub:

- **Each waiter carries its own executor** (`executor_ref ex_` in the awaiter) and
  is resumed by *posting* its continuation there. That is the pattern to copy: a
  publisher never runs a subscriber's code on the publisher's thread.
- **`async_event` is not a cross-thread primitive**, although the comment in
  `runtime.hpp` calls it "the primitive cross-thread completion signaling goes
  through". Either publishers and subscribers all live on one strand, or the topic
  needs its own cross-thread waiter list. This is worth a separate fix to the
  comment whichever way we go.

## The rules it must obey (zetta `CLAUDE.md`, first principle)

- **Coroutine-based:** "never a blocking mutex/condition-variable surface". A
  raw `std::mutex` is allowed only as the lowest cross-thread handoff, contained
  and commented, never in a public API. **Steve overrode this for pub/sub on
  2026-09-27** ([D1](decisions.md)): locks and condition variables are allowed
  inside the topic, and stage 1 does not use coroutines at all: subscribers are
  threads, with a mutex per queue ([D5](decisions.md)).
- **Distributed:** "single-node now, distribute later" is valid only as a
  reference implementation *with the distributed path designed behind the same
  API*. Stage 1 can be in-memory on one node, but its API has to be one a
  cluster-wide topic can also implement.
- **We write our own:** no moodycamel, folly or Disruptor dependency; we implement
  the primitive, with a test suite.
