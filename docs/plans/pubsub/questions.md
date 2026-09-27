# Open questions

Answered questions stay here with their answer, date and who answered; the answer
is also carried into the page it governs.

## Round 1 — 2026-09-27

Steve answered in one message, which settled [D1–D4](decisions.md) and moved the
design past several of these questions. Still open from round 1:

1. **What is the first thing that will publish, and the first thing that will
   subscribe?** Still open; it sets the defaults (inline size, segment size).
2. **Broadcast, work queue, or both?** → *Both, and more, chosen at runtime* (D3).
3. **When a ring is full?** → *Chain another ring* (D2). What happens when
   memory is the limit is Q13.
4. **Ordering per topic or per key?** → Reordering is a runtime choice (D3);
   per-key ordering still open.
5. **What is a message?** → *Binary with an encoding tag: blob + MIME type, JSON,
   …; heap for events too big for the ring* (D4).
6. Topics static in code or named at runtime and listable? Open.
7. Does a new subscriber see only new events, or the last N? Open.
8. Exposed on MCP/REST in stage 1, or engine-internal first? Open.
9. Lock-free a goal? → *No; locks and condition variables are fine* (D1).

## Round 2 — 2026-09-27

10. **How do coroutine subscribers wait?** A coroutine that blocks on a
    `condition_variable` holds a pool thread. Options: (a) thread subscribers use
    the condition variable, coroutine subscribers get a waiter list that
    `publish` posts to their executor (both, one mutex); (b) condition variable
    only, and coroutine code calls it through `run_blocking` on a pool thread;
    (c) condition variable only, and we accept the blocked thread.
11. **Where are delivery semantics chosen: per topic, per subscription, or per
    message?** E.g. is "deliver to one" a property of a subscriber group (like
    NATS queue groups), or can a single publish say "this one goes to 3
    recipients"?
12. **`std::queue` can't do broadcast.** It only has `front`/`pop`, so it can
    feed one reader. Is the `std::queue` store meant for work-queue topics only,
    or should the alternative store be a `std::deque` used by index?
13. **Is growth ever capped?** A slow or dead subscriber pins every ring after
    its cursor. Options: no cap; a per-topic memory limit after which the
    slowest subscriber is dropped or skipped ahead and told it missed N; or
    fall back to blocking the publisher.
14. **Heap events: immutable and refcounted, released when the last entitled
    subscriber passes them?** And is "too big for the ring" a per-topic
    inline threshold (e.g. 256 bytes), or literally larger than a whole ring
    segment?
15. **Which encodings in stage 1, and is the MIME type interned** to a small id
    in the header, or stored as a string per event?
16. **What does QoS mean for you: priority (who goes first), or a delivery
    guarantee** (at-most-once vs at-least-once with acknowledgement and
    redelivery, like MQTT QoS 0/1)? Or both?
