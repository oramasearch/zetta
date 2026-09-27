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

10. ~~**How do coroutine subscribers wait?**~~ → *Answered 2026-09-27 (Steve):
    don't use coroutines for this; subscribers wait on condition variables, with
    a mutex per queue, deque or consumer queue* ([D5](decisions.md)). Original
    question: A coroutine that blocks on a
    `condition_variable` holds a pool thread. Options: (a) thread subscribers use
    the condition variable, coroutine subscribers get a waiter list that
    `publish` posts to their executor (both, one mutex); (b) condition variable
    only, and coroutine code calls it through `run_blocking` on a pool thread;
    (c) condition variable only, and we accept the blocked thread.
11. ~~**Where are delivery semantics chosen?**~~ → *Answered 2026-09-27
    (Steve): at any level, or several* ([D6](decisions.md)). Original question:
    per topic, per subscription, or per message? E.g. is "deliver to one" a property of a subscriber group (like
    NATS queue groups), or can a single publish say "this one goes to 3
    recipients"?
12. **`std::queue` can't do broadcast.** It only has `front`/`pop`, so it can
    feed one reader. Is the `std::queue` store meant for work-queue topics only,
    or should the alternative store be a `std::deque` used by index?
13. ~~**Is growth ever capped?**~~ → *Answered 2026-09-27 (Steve): yes,
    configurable at all levels with sane defaults, potentially overflowing to
    disk* ([D10](decisions.md)). Original question: **Is growth ever capped?** A slow or dead subscriber pins every ring after
    its cursor. Options: no cap; a per-topic memory limit after which the
    slowest subscriber is dropped or skipped ahead and told it missed N; or
    fall back to blocking the publisher.
14. **Heap events: immutable and refcounted, released when the last entitled
    subscriber passes them?** And is "too big for the ring" a per-topic
    inline threshold (e.g. 256 bytes), or literally larger than a whole ring
    segment?
15. **Which encodings in stage 1, and is the MIME type interned** to a small id
    in the header, or stored as a string per event?
16. ~~**What does QoS mean?**~~ → *Answered 2026-09-27 (Steve): both priority
    and the delivery guarantee, configurable at all levels* ([D9](decisions.md)).
    Original question: **What does QoS mean for you: priority (who goes first), or a delivery
    guarantee** (at-most-once vs at-least-once with acknowledgement and
    redelivery, like MQTT QoS 0/1)? Or both?

## Round 3 — 2026-09-27

17. ~~**When levels disagree, which wins?**~~ → *Answered 2026-09-27 (Steve):
    the security/governance rules defined on the topic, user and subscriber,
    through the central governance model of users, roles, grants, rules and
    permissions* ([D7](decisions.md), [`governance.md`](governance.md)).
    Original question: Options: (a) the most specific wins
    (message over subscription over topic); (b) the topic and subscription set
    limits and a message can only narrow within them, the way MQTT delivers at
    the lower of the publish QoS and the subscription QoS; (c) a different rule
    per axis (e.g. recipients: message wins; priority: message wins; guarantee:
    the minimum).

## Round 4 — 2026-09-27

18. ~~**Over-bound requests: reject or clamp?**~~ → *Answered 2026-09-27
    (Steve): configurable per topic, clamp by default* ([D8](decisions.md)).
    Original question: **When a publish or subscription asks for more than governance allows** (e.g.
    priority 9 where the role's bound is 5, or "deliver to everyone" where it may
    only use "one"), do we (a) **reject** it with an error naming the rule, and
    audit the denial (zetta's deny-by-default), or (b) **clamp** it to the bound,
    deliver, and audit that it was clamped?

## Round 5 — 2026-09-27

19. **Are these the right defaults?** → *Steve, 2026-09-27: too small; a
    partition can see more than 10k events per second.* Revised in the stage 1
    page (limits in bytes, sized from a throughput target); the target itself
    is Q20. Original proposal: Proposed in
    [`stage-1-in-memory-fifo.md`](stage-1-in-memory-fifo.md#limits-and-overflow-d10):
    per-topic memory cap 64 MiB; a subscription may fall 10,000 events behind;
    when full, the default policy follows the guarantee: **at-most-once skips the
    slow subscriber ahead** (it is told how many it missed), **at-least-once and
    exactly-once spill to disk** up to 1 GiB per topic, then **hold the publisher**
    for up to 5 s, then fail the publish. Spilling is off for topics whose events
    must never touch disk.

## Round 6 — 2026-09-27

20. **What throughput should the defaults be sized for?** Proposed target: **100k
    events/s per partition at ~1 KiB each (~100 MiB/s)**. The revised defaults
    follow from it: 1 GiB of memory per partition (~10 s of traffic), 32 GiB of
    disk per partition (~5 min), both inside node-wide budgets (25% of RAM, 50% of
    free disk). If the target is higher, or events are bigger, the defaults scale
    with it.
