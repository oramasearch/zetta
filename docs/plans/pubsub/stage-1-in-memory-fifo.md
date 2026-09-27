# Stage 1 — an in-memory topic

**Status:** draft, revision 2 (after [D1–D4](decisions.md)). Settled parts cite
their decision; everything else is a proposal waiting on
[`questions.md`](questions.md).

## Shape

```
publish ──► [ topic ]
              mutex + condition_variable              (D1)
              event_store  ◄── pluggable               (D2)
                 default: segmented_ring = deque of vector rings
                 alt:     std::queue (std::deque)
              subscriptions: each has a cursor + delivery options (D3)
              message heap: events too big for a slot   (D4)
```

## Events (D4)

A slot in the ring holds a fixed-size header plus a small inline area:

```cpp
struct event_header {
    std::uint64_t sequence;   // assigned by the topic, monotonic
    std::uint32_t size;       // payload bytes
    encoding      kind;       // blob, json, ... (enum, extensible)
    std::uint16_t mime;       // interned MIME type id (blob); 0 = none
    std::uint8_t  priority;   // D3, if priority is per message
    std::uint8_t  flags;      // inline | heap
};
// payload: inline bytes when size <= inline_capacity, else an event_ptr
// to an immutable, refcounted event on the message heap.
```

Proposals here, each a question below: MIME types are **interned** to a 16-bit
id so a header stays small ([Q15](questions.md)); a heap event is **immutable
and refcounted**, so broadcast never copies it ([Q14](questions.md)).

## Storage (D2)

```cpp
class event_store {                       // runtime-chosen, so virtual
public:
    virtual ~event_store() = default;
    virtual std::uint64_t append(event) = 0;           // returns the sequence
    virtual const event* at(std::uint64_t seq) const = 0;
    virtual void release_before(std::uint64_t seq) = 0; // min cursor advanced
};
```

- **`segmented_ring`** (default): a `std::deque` of fixed-capacity
  `std::vector<slot>` rings. Full → push a new ring (from a small free list if
  one is spare). A ring is returned to the free list when every entitled cursor
  has passed its last sequence.
- **`std::queue`-backed**: a finding worth your eye — `std::queue` only exposes
  `front`/`pop`, so it can serve **one** reader (work queue), not broadcast with
  independent cursors. A `std::deque` used directly can, because it is indexable.
  See [Q12](questions.md).

## Subscribers and waiting (D1)

One `std::mutex` guards the store and the subscriber list. `publish` appends
under the lock, then wakes:

- **thread subscribers** wait on the topic's `std::condition_variable`
  (`notify_all`, each checks its own cursor);
- **coroutine subscribers** are held in a waiter list; `publish` posts each
  ready one to its own executor, so a coroutine never blocks a pool thread.

Whether we need both, or only one, is [Q10](questions.md).

## Delivery (D3)

Proposed options object, chosen at runtime:

```cpp
struct delivery {
    recipients who;        // all | one | n(k)
    ordering   order;      // fifo | priority (reordering allowed)
    // qos: priority, or a guarantee (at-most-once / at-least-once)? Q16
};
```

How this maps onto cursors:

- **all** (broadcast): each subscriber has its own cursor over the shared store.
- **one** (work queue): subscribers in a group share one cursor; each event is
  claimed once.
- **n(k)**: each event carries a remaining-claims count, decremented under the
  lock; the event is done at 0. Nobody surveyed ships this ([research/02](research/02-growable-rings-and-delivery.md)).
- **priority**: one ring per priority level, the reader takes the highest
  non-empty level; needs aging or low priority can starve.
